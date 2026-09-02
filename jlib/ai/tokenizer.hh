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
 * **Which one a file means is a property of the file, and the two known cases
 * are exact complements.**
 *
 * TinyLlama carries 61249 merges beginning `▁ t`, `e r`, `i n` -- the most
 * frequent pairs in English in descending frequency, which is what a BPE
 * merge table is -- and **every one of its 32000 scores is zero**. Not
 * approximately zero and not zero for the control tokens alone: checked
 * against the raw bytes, the array is 128000 zero bytes. Score-driven merging
 * cannot work on it at all, because every candidate ties.
 *
 * Gemma 2 carries **no merge list** and 256000 real scores. They are ranks
 * rather than log-probabilities -- score is `-(index - 473)` across the whole
 * ordinary vocabulary, the offset being the specials and byte tokens ahead of
 * it -- so the highest-scoring pair is the earliest one in the vocabulary,
 * and that is the signal the file has.
 *
 * So this reads whichever the file actually carries, and refuses a file with
 * neither. Scores are taken as usable only when they are not all zero, which
 * is the test that separates the two cases above.
 *
 * That the merge reading is right is not an inference: `encode("Hello world")`
 * gives `[1, 15043, 3186]`, which is the published Llama tokenization, and the
 * whole test suite rests on agreeing with ids that were established
 * independently. The score reading is checked the same way, against Gemma's
 * own reference tokenizer.
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
    /**
     * Which convention the vocabulary was built under.
     *
     * Not the architecture: these are independent axes, and assuming
     * otherwise is wrong in both directions.  Llama 3.2 is a llama-
     * architecture file with a gpt2 vocabulary; Gemma 2 is a gemma2 file with
     * a llama one.  `tokenizer.ggml.model` is what says which.
     *
     * **sentencepiece** marks a space with U+2581 and puts one in front of
     * the whole input -- the dummy prefix -- so "Hello" and " Hello" tokenize
     * alike.  A byte with no token of its own is spelled `<0xNN>`.
     *
     * **byte_level** is GPT-2's: every input byte is first mapped to a
     * printable character, so there is no byte without a token and no dummy
     * prefix at all.  A space is U+0120, and the vocabulary holds the mapped
     * characters rather than the bytes.
     */
    enum class flavour { sentencepiece, byte_level };

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

    /** Which convention this vocabulary was built under. */
    flavour convention() const { return m_flavour; }

    /**
     * Which signal drives the merges; see the note at the top.
     *
     * A property of the file rather than of the flavour: TinyLlama and Gemma
     * are both `llama` vocabularies and are driven the opposite ways.
     */
    enum class driver { merges, scores };

    driver driven_by() const { return m_driver; }

    /**
     * Whether a prompt should open with bos(), from
     * `tokenizer.ggml.add_bos_token`.
     *
     * A separate question from whether the vocabulary *has* a bos token, and
     * the two disagree: Qwen 2.5 names 151643 as bos and says not to add it,
     * because its chat template opens with `<|im_start|>system` and an
     * `<|endoftext|>` in front of that is an end-of-document marker at the
     * start of every conversation.
     *
     * Absent means yes, which is what Llama and TinyLlama are: they do not
     * carry the key and they do want one.
     *
     * `encode()` does **not** consult this -- its add_bos argument means what
     * it says, and the fixtures depend on that.  `chat::encode`, which is
     * what assembles a real prompt, does.
     */
    bool adds_bos() const { return m_adds_bos; }

    /**
     * Whether a leading word gets the space marker, from
     * `tokenizer.ggml.add_space_prefix`.
     *
     * SentencePiece's dummy prefix, and not every file wants it: Gemma 2 says
     * no, so "Hello" is the unmarked token and " Hello" the marked one, where
     * on Llama both are the marked one. decode() follows -- it strips a
     * leading space only where encode() put one, or it would eat a space the
     * text really had.
     */
    bool adds_space_prefix() const { return m_adds_space; }

    /**
     * Which cut the file named, from `tokenizer.ggml.pre`.
     *
     * Empty for a sentencepiece vocabulary, which does not have one, and for
     * byte-level files old enough to predate anyone writing it down.
     */
    const std::string& pre() const { return m_pre; }

    /**
     * Text to token ids.
     *
     * Under a **sentencepiece** vocabulary, a character with no token of its
     * own becomes its UTF-8 bytes, each as a `<0xXX>` token -- so any input
     * encodes, and nothing silently becomes `<unk>`.
     *
     * Under a **byte_level** one there is nothing to fall back to and nothing
     * to fall back for: every byte is mapped to a character the vocabulary
     * holds before the merges run, so no input is unencodable.  A symbol with
     * no id there means the merges produced something the vocabulary does not
     * contain, which is a broken file rather than a rare input, and it throws
     * rather than quietly encoding as something else.
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
     * as though the model had said it. `chat::encode` does exactly that: it
     * tokenizes the template's own text and the user's separately, passing
     * this flag per span, so a marker a user types is spelled out rather than
     * obeyed. A caller assembling a prompt by hand still has to.
     *
     * Cost is quadratic in the number of symbols: each merge rescans for the
     * best remaining pair. A priority queue over the pairs would make it
     * roughly linear, which llama.cpp does and this does not, because a prompt
     * is short and being able to read this one matters more. Measured at about
     * a millisecond for the twenty-token prompt in the tests.
     */
    std::vector<int> encode(const std::string& text, bool add_bos = true,
                            bool parse_special = true) const;

    /**
     * Append one run of text to a sequence already being built.
     *
     * For a caller assembling a prompt out of parts that need different
     * treatment -- a chat layout, where the markers are the template's and the
     * content is a stranger's, and only the markers may be allowed to mean
     * anything. See chat::encode.
     *
     * @param add_prefix the space marker in front, which belongs to the start
     *        of the whole input and so to the first run only
     * @param parse_special whether a control token spelled out in this run
     *        becomes that token, or the characters that spell it
     */
    void append(const std::string& text, bool add_prefix, bool parse_special,
                std::vector<int>& out) const;

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

    /** Which convention the file said, read from tokenizer.ggml.model. */
    flavour m_flavour = flavour::sentencepiece;

    /** tokenizer.ggml.pre, empty when the file does not say. */
    std::string m_pre;

    /** tokenizer.ggml.add_bos_token, defaulting to yes when absent. */
    bool m_adds_bos = true;

    /** tokenizer.ggml.add_space_prefix, defaulting to yes when absent. */
    bool m_adds_space = true;

    driver m_driver = driver::merges;

    /** tokenizer.ggml.scores, kept only when they are the file's signal. */
    std::vector<float> m_scores;

    /**
     * How good a merge of these two would be, lower being better, or false
     * if they do not merge at all.
     *
     * The one place the two drivers differ.  Under merges the key is the
     * pair's rank; under scores it is the *negated* score of the token the
     * pair would make, since a higher score is a better merge and this
     * compares the other way round.
     */
    bool merge_key(const std::string& left, const std::string& right,
                   double& key) const;

    std::string unmap(const std::string& t) const;

    // The merge table run over one prepared run of text -- the half of
    // encode_run that is the same either way, once each convention has done
    // what it does to the bytes first.
    void merge_run(const std::string& prepared, std::vector<int>& out) const;

    // GPT-2's byte-to-character map and its inverse, empty unless this is a
    // byte_level vocabulary.
    std::vector<std::string> m_byte_char;
    std::map<std::string, unsigned char> m_char_byte;

    int m_bos = -1;
    int m_eos = -1;
    int m_unk = -1;
};

}
}

#endif // JLIB_AI_TOKENIZER_HH
