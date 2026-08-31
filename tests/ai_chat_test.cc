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

#include <jlib/ai/chat.hh>

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

/** Newlines shown, so a difference in them is visible in the output. */
static std::string shown(const std::string& s) {
    std::string out;

    for(std::size_t i = 0; i < s.size(); i++) {
        if(s[i] == '\n') out += "\\n";
        else out += s[i];
    }

    return out;
}

/** TinyLlama's, as it appears in the file. */
static const char* ZEPHYR =
    "{% for message in messages %}\n"
    "{% if message['role'] == 'user' %}\n"
    "{{ '<|user|>\n' + message['content'] + eos_token }}\n"
    "{% elif message['role'] == 'system' %}\n"
    "{{ '<|system|>\n' + message['content'] + eos_token }}\n"
    "{% elif message['role'] == 'assistant' %}\n"
    "{{ '<|assistant|>\n'  + message['content'] + eos_token }}\n"
    "{% endif %}\n"
    "{% if loop.last and add_generation_prompt %}\n"
    "{{ '<|assistant|>' }}\n"
    "{% endif %}\n"
    "{% endfor %}";

static void it_reads_the_markers_out_of_the_template() {
    std::cout << "\nit reads the markers out of the template:\n";

    const ai::chat c(ZEPHYR, "</s>");

    ok("  three roles, in the order the template names them",
       c.roles() == std::vector<std::string>({ "user", "system", "assistant" }),
       std::to_string(c.roles().size()));

    ok("  each with its own marker",
       c.marker("user") == "<|user|>" && c.marker("system") == "<|system|>" &&
       c.marker("assistant") == "<|assistant|>");

    ok("  and a role it does not name has none", c.marker("tool").empty());
}

/**
 * A marker that is not a turn is carried along rather than filtered out.
 *
 * Documented behaviour rather than an accident: what decides whether a
 * template can be read is that it names user and assistant, and everything
 * else it mentions is simply available. Filtering candidates by how they look
 * was tried and removed -- see chat.cc -- because it rejected nothing the
 * user/assistant check did not already catch.
 */
static void a_stray_marker_is_harmless() {
    std::cout << "\na stray marker is harmless:\n";

    const ai::chat c(
        "{% for m in messages %}{{ '<|user|>\n' + m['content'] + '<|endoftext|>' }}"
        "{{ '<|assistant|>\n' }}{% endfor %}", "</s>");

    ok("  it is offered as a role", c.marker("endoftext") == "<|endoftext|>");

    // And changes nothing about laying out the turns that matter.
    ok("  and the conversation is laid out as usual",
       c.format({ { "user", "Hi" } }) == "<|user|>\nHi</s>\n<|assistant|>\n",
       shown(c.format({ { "user", "Hi" } })));
}

static void it_lays_a_turn_out() {
    std::cout << "\nit lays a turn out:\n";

    const ai::chat c(ZEPHYR, "</s>");

    const std::string one = c.format({ { "user", "What is the capital of Italy?" } });

    ok("  a question ends at the assistant's marker",
       one == "<|user|>\nWhat is the capital of Italy?</s>\n<|assistant|>\n",
       shown(one));

    // Which is the whole point: without the trailing marker the model is still
    // inside the user's turn and continues the question.
    const std::string bare = c.format({ { "user", "Hello" } }, false);

    ok("  and without the generation prompt it does not",
       bare == "<|user|>\nHello</s>\n", shown(bare));

    const std::string three = c.format({ { "system", "Be brief." },
                                         { "user", "Hi" },
                                         { "assistant", "Hello." },
                                         { "user", "Again?" } });

    ok("  a whole conversation keeps its order",
       three == "<|system|>\nBe brief.</s>\n"
                "<|user|>\nHi</s>\n"
                "<|assistant|>\nHello.</s>\n"
                "<|user|>\nAgain?</s>\n"
                "<|assistant|>\n",
       shown(three));

    bool threw = false;
    try { c.format({ { "moderator", "no such role" } }); }
    catch(ai::chat::exception&) { threw = true; }

    ok("  and a role the template never named is refused", threw);
}

/**
 * A template it cannot read is refused, not guessed at.
 *
 * This is the assertion the whole design turns on. A mis-laid conversation
 * does not fail visibly -- the model answers a question nobody asked -- so the
 * failure has to happen at construction, where it can be seen.
 */
static void a_template_it_cannot_read_is_refused() {
    std::cout << "\na template it cannot read is refused:\n";

    struct { const char* what; const char* tmpl; } cases[] = {
        { "ChatML, whose markers are not role names",
          "{% for m in messages %}{{ '<|im_start|>' + m['role'] + '\n' + "
          "m['content'] + '<|im_end|>' }}{% endfor %}" },
        { "Llama 2, which brackets instead",
          "{% for m in messages %}{{ '[INST] ' + m['content'] + ' [/INST]' }}"
          "{% endfor %}" },
        { "a template naming only a system prompt",
          "{{ '<|system|>\n' + system }}" },
        { "and one with no markers at all",
          "{% for m in messages %}{{ m['content'] }}{% endfor %}" }
    };

    for(const auto& e : cases) {
        bool threw = false;
        std::string why;

        try { ai::chat c(e.tmpl, "</s>"); }
        catch(ai::chat::exception& x) { threw = true; why = x.what(); }

        ok(std::string("  ") + e.what, threw);

        // And the message says what it looked for, since somebody hitting this
        // needs to know whether to write a template reader or a different one.
        if(threw)
            ok("    saying what it wanted",
               why.find("<|user|>") != std::string::npos &&
               why.find("chat.hh") != std::string::npos);
    }
}

/** And against the real file, which is where the markers actually come from. */
static bool exists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);

    return bool(f);
}

static void the_file_says_the_same(int argc, char** argv) {
    std::string path;

    if(argc > 1 && exists(argv[1])) path = argv[1];
    else if(const char* e = std::getenv("JLIB_GGUF")) { if(exists(e)) path = e; }

    if(path.empty()) {
        const char* names[] = {
            "tinyllama-1.1b-chat-v1.0.Q8_0.gguf",
            "../tinyllama-1.1b-chat-v1.0.Q8_0.gguf",
            "../../tinyllama-1.1b-chat-v1.0.Q8_0.gguf",
            "../../../tinyllama-1.1b-chat-v1.0.Q8_0.gguf"
        };

        for(const char* n : names) if(exists(n)) { path = n; break; }
    }

    if(path.empty()) {
        std::cout << "\n  (no model file, so the template is only exercised "
                  << "from the copy above)\n";

        return;
    }

    std::cout << "\nthe file says the same:\n";

    const ai::gguf g(path);
    const ai::chat c(g);

    ok("  the same three roles come out of the real template",
       c.roles() == std::vector<std::string>({ "user", "system", "assistant" }));

    ok("  and the layout matches the one built from the copy",
       c.format({ { "user", "Hi" } }) ==
       ai::chat(ZEPHYR, "</s>").format({ { "user", "Hi" } }));
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;

    it_reads_the_markers_out_of_the_template();
    it_lays_a_turn_out();
    a_stray_marker_is_harmless();
    a_template_it_cannot_read_is_refused();
    the_file_says_the_same(argc, argv);

    // What a green run does not establish.
    //
    // That the layout is byte-for-byte what Jinja would produce.  It was read
    // off the template by hand -- marker, newline, content, eos, newline --
    // and checked the only way that matters here, by asking the model a
    // question and getting an answer rather than a continuation.  A stray
    // newline would probably still answer, and nothing here would notice.
    //
    // That any template outside the Zephyr family works.  Four are checked to
    // be *refused*, which is the claim being made; none is checked to work.
    //
    // And nothing about the roles beyond their names.  A template that used
    // <|user|> to mean something else would pass every assertion here.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
