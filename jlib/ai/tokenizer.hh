/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#ifndef JLIB_AI_TOKENIZER_HH
#define JLIB_AI_TOKENIZER_HH

#include <jlib/ai/gguf.hh>

#include <exception>
#include <map>
#include <string>
#include <vector>

namespace jlib {
namespace ai {

/**
 * Byte-pair encoding against a vocabulary carried in a GGUF file.
 *
 * ### Merges, not scores, and why
 *
 * A Llama vocabulary can be driven two ways. SentencePiece merges by
 * **score** -- take whichever adjacent pair produces the highest-scoring token
 * -- and byte-pair encoding merges by **rank**, applying an ordered list of
 * learned pairs from the most frequent down. A GGUF can carry both:
 * `tokenizer.ggml.scores` and `tokenizer.ggml.merges`.
 *
 * This uses the merges, because in the file it was written against **every one
 * of the 32000 scores is zero**. Not approximately zero and not zero for the
 * control tokens alone -- checked against the raw bytes, the array is 128000
 * zero bytes. Score-driven merging cannot work at all on such a file: every
 * candidate pair ties, and what happens next is whatever the tie-break happens
 * to be rather than what the vocabulary meant.
 *
 * The merges are real. There are 61249 of them and they begin `▁ t`, `e r`,
 * `i n` -- the most frequent pairs in English, in descending frequency, which
 * is exactly what a BPE merge table is. So rank is the file's actual signal
 * and score is vestigial.
 *
 * That this is the right reading is not an inference: `encode("Hello world")`
 * gives `[1, 15043, 3186]`, which is the published Llama tokenization, and the
 * whole test suite rests on agreeing with ids that were established
 * independently.
 *
 * ### The dummy prefix
 *
 * Spaces become U+2581 (▁) and one is prepended to the text, so "Hello" and
 * " Hello" tokenize alike and a word carries its own leading space. decode()
 * undoes both. This is SentencePiece's convention and the vocabulary is built
 * for it -- `▁The` is a token and `The` is a different one.
 */
class tokenizer {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg)
            : m_msg("jlib::ai::tokenizer::exception: " + msg) {}

        const char* what() const throw() { return m_msg.c_str(); }

    private:
        std::string m_msg;
    };

    /** The token types GGUF gives, of which only BYTE and CONTROL matter here. */
    enum type { undefined = 0, normal = 1, unknown = 2, control = 3,
                user_defined = 4, unused = 5, byte = 6 };

    explicit tokenizer(const gguf& g);

    std::size_t size() const { return m_tokens.size(); }

    const std::string& token(int id) const;
    type token_type(int id) const;

    /** The id of an exact piece, or -1. */
    int id_of(const std::string& piece) const;

    int bos() const { return m_bos; }
    int eos() const { return m_eos; }
    int unk() const { return m_unk; }

    /**
     * Text to token ids.
     *
     * A character with no token of its own becomes its UTF-8 bytes, each as a
     * `<0xXX>` token -- so any input encodes, and nothing silently becomes
     * `<unk>`.
     *
     * ### Special tokens
     *
     * With `parse_special`, a control token written out in the text -- `</s>`
     * -- becomes that token rather than the characters that spell it. That
     * matters more than it sounds: a chat template closes every turn with
     * `</s>`, and without this the model is handed `</`, `s`, `>` and learns
     * from its own context that a reply ends by *typing* those characters. It
     * then does, which is a bug you can read in the output.
     *
     * Note what this does **not** cover. `<|user|>` is not in a Llama
     * vocabulary at all, so it is byte-pair encoded like any other text --
     * correctly, since that is the byte sequence the model was tuned on.
     * Only tokens the vocabulary actually has are recognised.
     *
     * **Turn it off for anything a stranger wrote.** Text containing `</s>`
     * from a user would otherwise end the turn early and let the rest be read
     * as though the model had said it. jchat does not do this yet -- it
     * tokenizes a whole formatted prompt in one call -- and doing it properly
     * means tokenizing the markers and the content separately.
     *
     * Cost is quadratic in the number of symbols: each merge rescans for the
     * best remaining pair. A priority queue over the pairs would make it
     * roughly linear, which llama.cpp does and this does not, because a prompt
     * is short and being able to read this one matters more. Measured at about
     * a millisecond for the twenty-token prompt in the tests.
     */
    std::vector<int> encode(const std::string& text, bool add_bos = true,
                            bool parse_special = true) const;

    /** Ids back to text, undoing the space marker and the dummy prefix. */
    std::string decode(const std::vector<int>& ids) const;

    /**
     * One token's text, with the space marker expanded and **nothing
     * stripped**.
     *
     * For streaming, where decode() is the wrong tool: it removes a leading
     * space, because encode() put one there as the dummy prefix, and that is
     * right exactly once at the start of a whole reply. Called per token it
     * would eat the space in front of every word.
     *
     * A control token yields the empty string, as it does in decode().
     */
    std::string piece(int id) const;

private:
    /** How many bytes the UTF-8 character starting here occupies. */
    static std::size_t char_len(const std::string& s, std::size_t at);

    /**
     * Byte-pair encode one run of ordinary text, appending to out.
     *
     * add_prefix only for the run that starts the input.  The dummy prefix
     * belongs to the text as a whole, and giving one to every run either side
     * of a control token inserts spaces that nobody wrote -- "a</s>b" came
     * back as "a b" while this took its argument for granted.
     */
    void encode_run(const std::string& text, bool add_prefix,
                    std::vector<int>& out) const;

    std::vector<std::string> m_tokens;
    std::vector<type> m_types;

    std::map<std::string, int> m_by_piece;

    /** "left right" as it appears in the file -> its rank, lowest first. */
    std::map<std::string, int> m_rank;

    /** The 256 byte tokens, by byte value, so fallback is a lookup. */
    std::vector<int> m_byte_token;

    int m_bos = -1;
    int m_eos = -1;
    int m_unk = -1;
};

}
}

#endif // JLIB_AI_TOKENIZER_HH
