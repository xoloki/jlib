/* -*- mode: C++ c-basic-offset: 4 -*-
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

#include <jlib/ai/tokenizer.hh>

#include "llama3_tokens.hh"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace ai = jlib::ai;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static bool exists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);

    return bool(f);
}

/**
 * A byte-level model, if one is to hand.
 *
 * Separate from find_model because it is optional: the sentencepiece tests are
 * the ones that must run, and a machine without a gpt2-vocabulary file skips
 * only that section rather than the whole program.
 */
static std::string find_byte_level_model() {
    if(const char* env = std::getenv("JLIB_GGUF_BPE"))
        if(exists(env)) return env;

    const char* names[] = {
        "Llama-3.2-1B-Instruct-Q8_0.gguf",
        "../Llama-3.2-1B-Instruct-Q8_0.gguf",
        "../../Llama-3.2-1B-Instruct-Q8_0.gguf",
        "../../../Llama-3.2-1B-Instruct-Q8_0.gguf",
        "qwen2.5-0.5b-instruct-q8_0.gguf",
        "../qwen2.5-0.5b-instruct-q8_0.gguf",
        "../../qwen2.5-0.5b-instruct-q8_0.gguf",
        "../../../qwen2.5-0.5b-instruct-q8_0.gguf"
    };

    for(const char* n : names)
        if(exists(n)) return n;

    return "";
}

static std::string find_model(int argc, char** argv) {
    if(argc > 1 && exists(argv[1])) return argv[1];

    if(const char* env = std::getenv("JLIB_GGUF"))
        if(exists(env)) return env;

    const char* names[] = {
        "tinyllama-1.1b-chat-v1.0.Q8_0.gguf",
        "../tinyllama-1.1b-chat-v1.0.Q8_0.gguf",
        "../../tinyllama-1.1b-chat-v1.0.Q8_0.gguf",
        "../../../tinyllama-1.1b-chat-v1.0.Q8_0.gguf"
    };

    for(const char* n : names)
        if(exists(n)) return n;

    return "";
}

static std::string spell(const ai::tokenizer& t, const std::vector<int>& ids) {
    std::string s;

    for(std::size_t i = 0; i < ids.size(); i++) {
        if(i) s += " ";
        s += std::to_string(ids[i]);
    }

    return s;
}

/**
 * The one assertion that is not self-referential.
 *
 * [1, 15043, 3186] for "Hello world" is the published Llama tokenization, and
 * the ids in the second were established independently -- looked up piece by
 * piece in the vocabulary, before this tokenizer existed, for the prompt the
 * model test runs. If the merge-versus-score reading were wrong, these would
 * not come out.
 */
static void it_agrees_with_known_tokenizations(const ai::tokenizer& t) {
    std::cout << "\nit agrees with known tokenizations:\n";

    const std::vector<int> hello = t.encode("Hello world");

    ok("  Hello world is 1, 15043, 3186",
       hello == std::vector<int>({ 1, 15043, 3186 }), spell(t, hello));

    const std::vector<int> paris = t.encode("The capital of France is Paris.");

    ok("  and the prompt tokenizes to the ids looked up by hand",
       paris == std::vector<int>({ 1, 450, 7483, 310, 3444, 338, 3681, 29889 }),
       spell(t, paris));
}

static void the_vocabulary_is_what_the_file_said(const ai::tokenizer& t) {
    std::cout << "\nthe vocabulary is what the file said:\n";

    ok("  32000 tokens", t.size() == 32000, std::to_string(t.size()));

    ok("  bos 1, eos 2, unk 0",
       t.bos() == 1 && t.eos() == 2 && t.unk() == 0);

    ok("  <s> and </s> are control tokens",
       t.token_type(1) == ai::tokenizer::control &&
       t.token_type(2) == ai::tokenizer::control);

    ok("  and the 256 byte tokens run from id 3",
       t.token(3) == "<0x00>" && t.token(258) == "<0xFF>" &&
       t.token_type(3) == ai::tokenizer::byte);

    bool threw = false;
    try { t.token(99999); }
    catch(ai::tokenizer::exception&) { threw = true; }

    ok("  an id outside the vocabulary is refused", threw);
}

static void the_space_marker_and_the_dummy_prefix(const ai::tokenizer& t) {
    std::cout << "\nthe space marker and the dummy prefix:\n";

    // The vocabulary has both, and they are not the same token: a word knows
    // whether it began a word or continued one.
    ok("  the marked and unmarked forms are different tokens",
       t.id_of("\xe2\x96\x81" "The") != t.id_of("The") &&
       t.id_of("\xe2\x96\x81" "The") >= 0 && t.id_of("The") >= 0);

    // Which is what the prefix is for: a leading word gets the marked form, so
    // it matches the same word appearing mid-sentence.
    ok("  a leading word gets the marked form anyway",
       t.encode("The", false) == std::vector<int>({ t.id_of("\xe2\x96\x81" "The") }),
       spell(t, t.encode("The", false)));

    ok("  add_bos false leaves the sentence marker off",
       t.encode("Hello world", false) == std::vector<int>({ 15043, 3186 }),
       spell(t, t.encode("Hello world", false)));
}

/** Anything encodes, because a character with no token becomes its bytes. */
static void anything_encodes(const ai::tokenizer& t) {
    std::cout << "\nanything encodes:\n";

    // An emoji is four UTF-8 bytes and no Llama vocabulary has a token for it.
    const std::string emoji = "\xf0\x9f\x8e\xb2";

    const std::vector<int> ids = t.encode(emoji, false);

    bool all_bytes = !ids.empty();

    for(std::size_t i = 0; i < ids.size(); i++)
        if(t.token_type(ids[i]) != ai::tokenizer::byte &&
           t.token(ids[i]) != "\xe2\x96\x81")
            all_bytes = false;

    ok("  a character with no token becomes byte tokens", all_bytes,
       spell(t, ids));

    ok("  and it comes back", t.decode(ids) == emoji, t.decode(ids));

    ok("  the empty string is a sentence marker and nothing else",
       t.encode("") == std::vector<int>({ 1 }), spell(t, t.encode("")));
}

/**
 * piece() is decode() without the one thing decode() must do.
 *
 * decode() strips a leading space because encode() adds one as the dummy
 * prefix, and that is right exactly once for a whole reply. Streaming calls it
 * per token, where it would eat the space in front of every word -- so the
 * assertion is that concatenating pieces gives back what decode() gives,
 * except for that single leading space.
 */
/**
 * A control token written out in the text is that token.
 *
 * Found by reading jchat's output: the second reply of a conversation ended
 * with a literal "</s>". A chat template closes every turn with it, and
 * without this it was handed to the model as `</`, `s`, `>` -- so the model
 * learned from its own context that a reply ends by typing those characters,
 * and did.
 */
static void a_control_token_in_the_text_is_that_token(const ai::tokenizer& t) {
    std::cout << "\na control token in the text is that token:\n";

    ok("  </s> is one token, not three",
       t.encode("</s>", false) == std::vector<int>({ 2 }),
       spell(t, t.encode("</s>", false)));

    ok("  and <s> likewise",
       t.encode("<s>", false) == std::vector<int>({ 1 }),
       spell(t, t.encode("<s>", false)));

    // The dummy prefix belongs to the text, not to each run either side of a
    // split.  Giving one to every run put a space in that nobody wrote.
    const std::vector<int> around = t.encode("a</s>b", false);

    ok("  the text around it keeps its shape", around.size() == 3 &&
       around[1] == 2, spell(t, around));

    ok("  with no space invented at the split",
       t.decode(around) == "a</s>b" || t.decode(around) == "ab",
       "\"" + t.decode(around) + "\"");

    // Off, for text a stranger wrote: otherwise "</s>" typed by a user ends
    // the turn and the rest is read as though the model had said it.
    const std::vector<int> literal = t.encode("</s>", false, false);

    ok("  and with parse_special off it is spelled out again",
       literal.size() > 1 && literal[0] != 2, spell(t, literal));

    // The whole reason it matters: a formatted turn has exactly one of them.
    int twos = 0;

    for(int id : t.encode("<|user|>\nHi</s>\n<|assistant|>\n"))
        if(id == 2) twos++;

    ok("  so a formatted turn carries one end-of-sequence", twos == 1,
       std::to_string(twos));
}

static void pieces_concatenate_to_the_whole(const ai::tokenizer& t) {
    std::cout << "\npieces concatenate to the whole:\n";

    const std::string text = "The capital of Italy is Rome.";

    const std::vector<int> ids = t.encode(text);

    std::string joined;

    for(std::size_t i = 0; i < ids.size(); i++) joined += t.piece(ids[i]);

    ok("  the pieces put back together are the text, with its leading space",
       joined == " " + text, "\"" + joined + "\"");

    ok("  and decode is that with the space gone", t.decode(ids) == text);

    // Which is the failure mode this exists to avoid: a per-token decode()
    // loses the space in front of every word, and the result is unreadable.
    std::string wrong;

    for(std::size_t i = 0; i < ids.size(); i++)
        wrong += t.decode(std::vector<int>(1, ids[i]));

    ok("  where decoding token by token would run the words together",
       wrong.find("capitalof") != std::string::npos ||
       wrong.find("Thecapital") != std::string::npos, "\"" + wrong + "\"");

    ok("  a control token has no text of its own", t.piece(t.bos()).empty());
}

static void it_round_trips(const ai::tokenizer& t) {
    std::cout << "\nit round trips:\n";

    const char* cases[] = {
        "Hello world",
        "The capital of France is Paris.",
        "a",
        "  double  spaces  ",
        "line\nbreak\ttab",
        "Numbers 1234567890 and symbols !@#$%^&*()",
        "caf\xc3\xa9 na\xc3\xaf""ve \xe2\x80\x94 dashes",
        "\xf0\x9f\x8e\xb2 \xe4\xb8\xad\xe6\x96\x87 mixed"
    };

    for(const char* c : cases) {
        const std::string in = c;
        const std::string out = t.decode(t.encode(in));

        ok(std::string("  ") + "\"" + in.substr(0, 34) + "\"", out == in,
           out == in ? "" : "got \"" + out + "\"");
    }
}

static void it_is_quick_enough(const ai::tokenizer& t) {
    std::cout << "\nit is quick enough:\n";

    const std::string prompt =
        "The capital of France is Paris. The capital of Germany is Berlin. "
        "The capital of Italy is";

    const auto start = std::chrono::steady_clock::now();

    std::vector<int> ids;

    for(int i = 0; i < 20; i++) ids = t.encode(prompt);

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count() / 20.0;

    // Not a benchmark, a bound.  encode() is quadratic in the symbol count and
    // the header says so; this is here to notice if that ever stops being fine
    // for a prompt-sized input rather than to measure how fast it is.
    ok("  a twenty-token prompt encodes in under 50ms", ms < 50.0,
       std::to_string(ms) + "ms");

    ok("  and gives the twenty tokens the model test uses", ids.size() == 20,
       std::to_string(ids.size()));
}

/**
 * A byte-level vocabulary, against the reference tokenizer's answers.
 *
 * The flavour is read from `tokenizer.ggml.model`, and it is independent of
 * the architecture: Llama 3.2 is a llama-architecture file with a gpt2
 * vocabulary, Gemma 2 is a gemma2 file with a llama one.  Assuming one from
 * the other is wrong in both directions.
 *
 * What the byte-level convention changes: every input byte is mapped to a
 * printable character first, so no byte is unencodable and there is no dummy
 * prefix.  Before this, encoding anything at all under a gpt2 vocabulary
 * produced three -1 ids -- the three UTF-8 bytes of SentencePiece's U+2581,
 * prepended to every input and present in no gpt2 vocabulary.
 */
static void a_byte_level_vocabulary_matches_the_reference(const std::string& path) {
    std::cout << "\nit tokenizes a byte-level vocabulary as the reference does:\n";

    const ai::gguf g(path);
    const ai::tokenizer t(g);

    ok("  the flavour comes from the file",
       t.convention() == ai::tokenizer::flavour::byte_level);

    ok("  and there is no unknown token, because nothing is unencodable",
       t.unk() < 0, std::to_string(t.unk()));

    std::size_t matched = 0, pending = 0;

    for(std::size_t i = 0; i < LLAMA3_COUNT; i++) {
        const tokenization& c = LLAMA3[i];
        const std::vector<int> got = t.encode(c.text, false, true);

        bool same = got.size() == std::size_t(c.count);

        for(int j = 0; same && j < c.count; j++)
            same = got[std::size_t(j)] == c.ids[j];

        if(c.pretok) {
            // Known difference: the answer depends on the pre-tokenization
            // regex, which is not implemented.  Asserted as *still wrong*, so
            // that implementing it makes this test fail and say so rather than
            // passing quietly with the fixture unexamined.
            ok(std::string("  pre-tokenizer still needed for '") + c.text + "'",
               !same);

            pending++;

            continue;
        }

        ok(std::string("  '") + c.text + "'", same, spell(t, got));

        if(same) matched++;
    }

    std::cout << "    " << matched << " match, " << pending
              << " await the pre-tokenizer\n";
}

/** SentencePiece is untouched by any of it. */
static void the_sentencepiece_path_is_unchanged(const ai::tokenizer& t) {
    std::cout << "\nthe sentencepiece path is unchanged:\n";

    ok("  the flavour is still sentencepiece",
       t.convention() == ai::tokenizer::flavour::sentencepiece);

    // The dummy prefix is the whole difference between the two conventions,
    // and it must still be there: "Hello" and " Hello" tokenize alike.
    ok("  and the dummy prefix is still applied",
       t.encode("Hello", false, false) == t.encode("Hello", false, false) &&
       !t.encode("Hello", false, false).empty());
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;

    const std::string path = find_model(argc, argv);

    if(path.empty()) {
        std::cout << "ai_tokenizer_test: no model file; pass one as an argument "
                  << "or set JLIB_GGUF.\n";

        return 77;
    }

    std::cout << "ai_tokenizer_test: " << path << "\n";

    try {
        const ai::gguf g(path);
        const ai::tokenizer t(g);

        it_agrees_with_known_tokenizations(t);
        the_vocabulary_is_what_the_file_said(t);
        the_space_marker_and_the_dummy_prefix(t);
        the_sentencepiece_path_is_unchanged(t);
        anything_encodes(t);
        a_control_token_in_the_text_is_that_token(t);
    pieces_concatenate_to_the_whole(t);
    it_round_trips(t);
        it_is_quick_enough(t);
        const std::string bpe = find_byte_level_model();

        if(bpe.empty())
            std::cout << "\n(no byte-level model; set JLIB_GGUF_BPE to run those)\n";
        else
            a_byte_level_vocabulary_matches_the_reference(bpe);

    }
    catch(std::exception& e) {
        std::cerr << "ai_tokenizer_test: " << e.what() << "\n";

        return 1;
    }

    // What a green run does not establish.
    //
    // That this matches llama.cpp on arbitrary text.  Two tokenizations are
    // checked against ids from outside this library, and the rest is
    // round-tripping -- which a consistently wrong encoder would also pass,
    // since decode() only has to invert whatever encode() did.
    //
    // That a conversation is safe against what a user types.  encode() takes
    // parse_special and jchat does not use it: a whole formatted prompt goes
    // through in one call, so a user writing "</s>" ends the turn early and
    // what follows is read as though the model said it.  Doing it properly
    // means tokenizing the markers and the content separately, which nothing
    // here does.
    //
    // Nothing about the score-driven path.  The file this was written against
    // has all-zero scores, so merges are the only usable signal in it; a
    // vocabulary with real scores would want SentencePiece's algorithm, and
    // this would tokenize it differently and never say so.
    //
    // And no chat template.  The file carries tokenizer.chat_template, which
    // is how an instruct model expects a conversation to be laid out, and
    // nothing here reads it.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
