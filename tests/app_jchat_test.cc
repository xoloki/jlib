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

#include <jlib/sys/sys.hh>
#include <jlib/sys/pstream.hh>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

static std::string find_one(const char* const* names, std::size_t n) {
    for(std::size_t i = 0; i < n; i++)
        if(exists(names[i])) return names[i];

    return "";
}

/**
 * The app itself, which is beside this test in the build tree.
 *
 * Not $PATH: an installed jchat from an older build would be tested instead of
 * the one just compiled, and it would pass.
 */
static std::string find_jchat() {
    const char* names[] = {
        "../jlib/apps/jchat", "./jlib/apps/jchat", "../../build/jlib/apps/jchat"
    };

    return find_one(names, sizeof(names) / sizeof(names[0]));
}

static std::string find_model() {
    if(const char* e = std::getenv("JLIB_GGUF")) if(exists(e)) return e;

    const char* names[] = {
        "tinyllama-1.1b-chat-v1.0.Q8_0.gguf",
        "../tinyllama-1.1b-chat-v1.0.Q8_0.gguf",
        "../../tinyllama-1.1b-chat-v1.0.Q8_0.gguf",
        "../../../tinyllama-1.1b-chat-v1.0.Q8_0.gguf"
    };

    return find_one(names, sizeof(names) / sizeof(names[0]));
}

/** These need no model, so they run wherever the app was built. */
static void it_explains_itself(const std::string& jchat) {
    std::cout << "\nit explains itself:\n";

    std::string out, err;

    const int rc = jlib::sys::run({ jchat, "--help" }, out, err);

    ok("  --help succeeds", rc == 0, std::to_string(rc));

    ok("  and says how to call it",
       out.find("usage:") != std::string::npos &&
       out.find("--system") != std::string::npos &&
       out.find("--raw") != std::string::npos);

    out.clear(); err.clear();

    const int bad = jlib::sys::run({ jchat, "--nonesuch", "x" }, out, err);

    ok("  an option it does not have is refused", bad != 0, std::to_string(bad));

    ok("  saying which one", err.find("--nonesuch") != std::string::npos, err);

    out.clear(); err.clear();

    const int none = jlib::sys::run({ jchat }, out, err);

    ok("  and with no model at all it prints the usage",
       none != 0 && err.find("usage:") != std::string::npos, std::to_string(none));
}

static void it_answers_from_the_command_line(const std::string& jchat,
                                             const std::string& model)
{
    std::cout << "\nit answers from the command line:\n";

    std::string out, err;

    const int rc = jlib::sys::run({ jchat, "--temp", "0", "--tokens", "40",
                                    model, "What is the capital of Italy?" },
                                  out, err);

    ok("  it runs", rc == 0, std::to_string(rc) + " " + err);

    ok("  and answers Rome", out.find("Rome") != std::string::npos,
       "\"" + out + "\"");

    // The reason the split exists: a caller redirecting stdout gets the answer
    // and none of the progress.
    ok("  with the timings on stderr and not in the answer",
       out.find("loaded in") == std::string::npos &&
       out.find("tokens in") == std::string::npos &&
       err.find("loaded in") != std::string::npos,
       "\"" + out + "\"");

    // Greedy, so the same question twice is the same answer.
    std::string again, ignored;

    jlib::sys::run({ jchat, "--temp", "0", "--tokens", "40",
                     model, "What is the capital of Italy?" }, again, ignored);

    ok("  and at temperature zero it repeats exactly", again == out);
}

/**
 * The conversation is kept, which is the whole reason it is not one-shot only.
 *
 * "And Italy?" is answerable only by something that remembers being asked
 * about a capital -- a fresh prompt has no idea what is being asked.
 */
static void it_remembers_the_conversation(const std::string& jchat,
                                          const std::string& model)
{
    std::cout << "\nit remembers the conversation:\n";

    // Through a file rather than down a pipe: sys::pstream is popen, so it
    // runs one direction at a time and has no way to say "that is all the
    // input" -- and without an end of input the conversation loop never
    // returns.  A redirect gives the child a stdin that ends.
    const std::string in = "jchat-turns.txt";

    {
        std::ofstream f(in.c_str());

        f << "What is the capital of France?\n"
          << "And Italy?\n";
    }

    // The shell is involved here, which library code would avoid -- sys::run
    // exists for exactly that reason.  In a test, with strings this file
    // wrote, a redirect is worth more than the principle.
    jlib::sys::pstream p(jchat + " --temp 0 --tokens 40 " + model +
                         " < " + in + " 2>&1", std::ios::in);

    std::string all;
    std::string line;

    while(std::getline(p, line)) all += line + "\n";

    p.close();

    std::remove(in.c_str());

    ok("  the first answer is Paris", all.find("Paris") != std::string::npos,
       "\"" + all + "\"");

    ok("  and the follow-up is answered from the history",
       all.find("Rome") != std::string::npos, "\"" + all + "\"");
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;

    const std::string jchat = argc > 1 && exists(argv[1]) ? argv[1] : find_jchat();

    if(jchat.empty()) {
        std::cout << "app_jchat_test: no jchat binary beside this test\n";

        return 77;
    }

    std::cout << "app_jchat_test: " << jchat << "\n";

    try {
        it_explains_itself(jchat);

        const std::string model = find_model();

        if(model.empty())
            std::cout << "\n  (no model file, so only the argument handling "
                      << "is exercised)\n";
        else {
            it_answers_from_the_command_line(jchat, model);
            it_remembers_the_conversation(jchat, model);
        }
    }
    catch(std::exception& e) {
        std::cerr << "app_jchat_test: " << e.what() << "\n";

        return 1;
    }

    // What a green run does not establish.
    //
    // Anything about what the model says beyond one word of it.  "Rome" is in
    // the answer; whether the sentence around it is good is not something a
    // test can hold, and a greedy run only pins it for this model at this
    // quantisation.
    //
    // Nothing about the interactive experience.  Input is piped here, so there
    // is no terminal, no editing and no interrupt handling -- and Ctrl-C part
    // way through a reply is exactly the case a person will hit first.
    //
    // And nothing about the CPU path.  These run on whatever backend jchat
    // chose, which where there is Metal is the GPU; the float fallback is
    // reached only on a machine without it.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
