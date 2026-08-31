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
     * Cost is quadratic in the number of symbols: each merge rescans for the
     * best remaining pair. A priority queue over the pairs would make it
     * roughly linear, which llama.cpp does and this does not, because a prompt
     * is short and being able to read this one matters more. Measured at about
     * a millisecond for the twenty-token prompt in the tests.
     */
    std::vector<int> encode(const std::string& text, bool add_bos = true) const;

    /** Ids back to text, undoing the space marker and the dummy prefix. */
    std::string decode(const std::vector<int>& ids) const;

private:
    /** How many bytes the UTF-8 character starting here occupies. */
    static std::size_t char_len(const std::string& s, std::size_t at);

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
