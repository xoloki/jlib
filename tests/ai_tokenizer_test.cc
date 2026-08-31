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
        anything_encodes(t);
        it_round_trips(t);
        it_is_quick_enough(t);
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
