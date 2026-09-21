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
 */

// What a model said, turned into files (#265).
//
// Recorded replies in, a temp tree out: no model, no socket, and every case
// below is a shape something actually sends. The leniency is deliberate --
// Joey's call -- and so is the requirement that every absorbed deviation shows
// up in edit::guesses, because a guess nobody can see is how the wrong file
// gets written and nobody learns why.
//
// The assertion worth more than the rest: a reply that stops inside a fence
// writes nothing at all.

#include <jlib/apps/jcode.hh>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

namespace jcode = jlib::apps::jcode;
namespace oa = jlib::ai::openai;
namespace ai = jlib::ai;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** A directory to write into, removed at the end. */
struct tree {
    std::string path;

    tree() {
        char t[] = "/tmp/jcode_test_XXXXXX";

        path = ::mkdtemp(t) ? t : "";
    }

    ~tree() {
        if(!path.empty()) {
            const std::string cmd = "rm -rf '" + path + "'";

            if(system(cmd.c_str())) {}
        }
    }

    void put(const std::string& name, const std::string& content) const {
        std::ofstream o(path + "/" + name, std::ios::binary);

        o << content;
    }

    std::string get(const std::string& name, bool& found) const {
        std::ifstream i(path + "/" + name, std::ios::binary);

        found = bool(i);

        std::ostringstream all;

        all << i.rdbuf();

        return all.str();
    }
};

static const std::vector<std::string> KNOWN{ "main.cc", "src/util.cc" };

static std::string fence(const std::string& name, const std::string& body) {
    return name + "\n```\n" + body + "```\n";
}

// ---------------------------------------------------------------- parsing

static void the_format_as_asked_for() {
    std::cout << "\na reply in the format it was asked for:\n";

    const jcode::reply r = jcode::parse(
        "Ok, I will change the greeting.\n\n" +
        fence("main.cc", "int main() { return 0; }\n") +
        "\nThat should do it.\n", KNOWN);

    ok("one edit", r.edits.size() == 1, std::to_string(r.edits.size()));

    ok("  naming the file", !r.edits.empty() && r.edits[0].name == "main.cc",
       r.edits.empty() ? "" : r.edits[0].name);

    ok("  with the whole content",
       !r.edits.empty() && r.edits[0].content == "int main() { return 0; }\n",
       r.edits.empty() ? "" : r.edits[0].content);

    ok("  and nothing guessed", !r.edits.empty() && r.edits[0].guesses.empty(),
       r.edits.empty() || r.edits[0].guesses.empty() ? ""
                                                     : r.edits[0].guesses[0]);

    ok("  and nothing refused", r.refusals.empty());
}

/**
 * A tool call is noticed rather than read as prose.
 *
 * #316. jcode declares no tools, so this is a model going somewhere it was not
 * sent -- but the markup means nothing to the edit parser, so without this the
 * reply reads as one containing no files, which is exactly what a model too
 * weak to follow the format produces. A user cannot tell those apart from
 * silence, and the whole point of the guess log (#265) is that jcode does not
 * decide quietly.
 */
/**
 * A reply that gives the same file several times writes it once.
 *
 * Measured, not imagined: asked to fix three functions, a model wrote six
 * illustrative snippets and then the file, and jcode announced seven writes of
 * util.c. apply() wrote them in order so the result was the last one anyway --
 * the other six were announcements of a write that did not survive.
 */
static void a_file_given_twice_is_written_once() {
    std::cout << "\na file given more than once:\n";

    const jcode::reply r = jcode::parse(
        "Here is the idea:\n\n" +
        fence("main.cc", "int main() { return 1; }\n") +
        "\nand here is the file:\n\n" +
        fence("main.cc", "int main() { return 0; }\n"), KNOWN);

    ok("one edit, not two", r.edits.size() == 1,
       std::to_string(r.edits.size()));

    // The last, because that is what writing them in order already produced
    // and because a model correcting itself means the correction.
    ok("  and it is the last one given",
       r.edits.size() == 1 && r.edits[0].content == "int main() { return 0; }\n",
       r.edits.empty() ? "" : r.edits[0].content);

    // Superseded rather than silently dropped: a reply needing this was not
    // in the format asked for, and the user should see how far off it was.
    ok("  with the superseded one refused, not forgotten",
       r.refusals.size() == 1 &&
       r.refusals[0].why.find("superseded") != std::string::npos,
       r.refusals.empty() ? "none" : r.refusals[0].why);

    // Different files are untouched by any of this.
    const jcode::reply two = jcode::parse(
        fence("main.cc", "a\n") + "\n" + fence("util.cc", "b\n"), KNOWN);

    ok("two different files are still two edits", two.edits.size() == 2,
       std::to_string(two.edits.size()));
}

static void a_tool_call_in_the_reply() {
    std::cout << "\na reply that asks to call something:\n";

    const jcode::reply only = jcode::parse(
        "<tool_call>\n{\"name\":\"read_file\",\"arguments\":{\"path\":\"a.c\"}}\n"
        "</tool_call>", KNOWN);

    ok("the call is seen", only.calls.size() == 1,
       std::to_string(only.calls.size()));

    ok("  by name", !only.calls.empty() && only.calls[0].name == "read_file",
       only.calls.empty() ? "" : only.calls[0].name);

    ok("  with its arguments",
       !only.calls.empty() &&
       only.calls[0].arguments.find("a.c") != std::string::npos,
       only.calls.empty() ? "" : only.calls[0].arguments);

    ok("  and no edit invented from it", only.edits.empty());

    // A model that does both gets both read: the call is lifted out and what
    // is left goes through the edit parser as usual.
    const jcode::reply both = jcode::parse(
        "First I will look.\n"
        "<tool_call>{\"name\":\"read_file\",\"arguments\":{}}</tool_call>\n"
        "Then this:\n\n" +
        fence("main.cc", "int main() { return 1; }\n"), KNOWN);

    ok("a reply with a call and an edit yields both",
       both.calls.size() == 1 && both.edits.size() == 1,
       std::to_string(both.calls.size()) + " calls, " +
       std::to_string(both.edits.size()) + " edits");

    ok("  and the edit is the one that was written",
       both.edits.size() == 1 &&
       both.edits[0].content == "int main() { return 1; }\n");

    // Markup that is not a call must not become one -- and must not vanish
    // either, since it is what the model actually said.
    const jcode::reply broken = jcode::parse(
        "<tool_call>not json</tool_call>", KNOWN);

    ok("markup that will not parse is not a call", broken.calls.empty());

    // An ordinary reply is untouched by any of this.
    const jcode::reply plain = jcode::parse(
        "Here you go.\n\n" + fence("main.cc", "int main() {}\n"), KNOWN);

    ok("and a reply with no markup carries no calls",
       plain.calls.empty() && plain.edits.size() == 1);
}

/**
 * The loop: a call is run, its result goes back, and the round ends.
 *
 * #328. No model, no server, no disk -- the exchange is a lambda handing back
 * scripted replies, which is most of why `converse` takes one rather than
 * speaking HTTP itself.
 */
static void the_agent_loop() {
    std::cout << "\nthe loop that answers a call:\n";

    // A tool that records what it was asked and answers.
    std::vector<std::string> asked;

    jcode::tool reader;

    reader.name = "read_file";
    reader.parameters = R"({"type":"object","properties":{"path":{"type":"string"}}})";
    reader.run = [&](const std::string& args) {
        asked.push_back(args);

        return std::string("int main(void){}");
    };

    // The script: first reply asks, second answers.
    auto call_then_answer = [](std::size_t& n) {
        return [&n](const std::vector<ai::message>&,
                    const std::string&) -> oa::answer {
            oa::answer a;

            if(n++ == 0) {
                oa::call c;

                c.id = "c1";
                c.name = "read_file";
                c.arguments = R"({"path":"main.c"})";

                a.calls.push_back(c);
                a.why = oa::finish::tool_calls;
            }
            else a.content = "Looks fine.";

            return a;
        };
    };

    std::size_t n = 0;

    const jcode::conversation c =
        jcode::converse({ { "user", "check main.c" } }, { reader },
                        call_then_answer(n));

    ok("it ends by being answered", c.why == jcode::ending::answered,
       jcode::spell(c.why));

    ok("  after two rounds", c.rounds == 2, std::to_string(c.rounds));

    ok("  having run the tool once", c.calls.size() == 1 && asked.size() == 1,
       std::to_string(c.calls.size()) + "/" + std::to_string(asked.size()));

    ok("  with the arguments the model gave",
       asked.size() == 1 && asked[0].find("main.c") != std::string::npos,
       asked.empty() ? "" : asked[0]);

    ok("  and the last reply is the prose", c.text == "Looks fine.", c.text);

    // **The assistant turn carries the call**, not only the result -- without
    // it the model is shown an answer to a question it cannot see itself
    // asking. #311 renders these into the model's own markup.
    bool carried = false;

    for(const ai::message& m : c.turns)
        if(m.role == "assistant" && !m.tool_calls.empty() &&
           m.tool_calls[0].name == "read_file") carried = true;

    ok("  and the conversation keeps the call the model made", carried);

    bool answered = false;

    for(const ai::message& m : c.turns)
        if(m.role == "tool" && m.content == "int main(void){}") answered = true;

    ok("  alongside the result it was given", answered);
}

/** The three ways it stops that are not "answered". */
static void the_loop_knows_why_it_stopped() {
    std::cout << "\nthe loop knows why it stopped:\n";

    jcode::tool forever;

    forever.name = "again";
    forever.run = [](const std::string&) { return std::string("ok"); };

    // A model that asks forever is the ordinary failure, not an exotic one.
    const auto always = [](const std::vector<ai::message>&,
                           const std::string&) -> oa::answer {
        oa::answer a;
        oa::call c;

        c.name = "again";
        c.arguments = "{}";

        a.calls.push_back(c);

        return a;
    };

    const jcode::conversation b =
        jcode::converse({ { "user", "go" } }, { forever }, always, 3);

    ok("a model that never stops is bounded", b.why == jcode::ending::bounded,
       jcode::spell(b.why));

    ok("  at the bound it was given", b.rounds == 3, std::to_string(b.rounds));

    ok("  and says so", b.detail.find("3") != std::string::npos, b.detail);

    // An exchange that throws is not an answer.
    const jcode::conversation f = jcode::converse(
        { { "user", "go" } }, { forever },
        [](const std::vector<ai::message>&, const std::string&) -> oa::answer {
            throw std::runtime_error("connection refused");
        });

    ok("an exchange that throws ends as failed", f.why == jcode::ending::failed,
       jcode::spell(f.why));

    ok("  carrying the reason", f.detail == "connection refused", f.detail);

    // A call for a tool nobody registered is refused *to the model*, so it is
    // told rather than left waiting -- and recorded rather than hidden.
    std::size_t rounds = 0;

    const jcode::conversation u = jcode::converse(
        { { "user", "go" } }, { forever },
        [&rounds](const std::vector<ai::message>& turns,
                  const std::string&) -> oa::answer {
            oa::answer a;

            if(rounds++ == 0) {
                oa::call c;

                c.name = "rm_rf";
                c.arguments = "{}";

                a.calls.push_back(c);
            }
            else {
                // The refusal came back as this call's result.
                for(const ai::message& m : turns)
                    if(m.role == "tool" &&
                       m.content.find("no tool called") != std::string::npos)
                        a.content = "saw the refusal";
            }

            return a;
        });

    ok("an unknown tool is refused rather than guessed",
       u.calls.size() == 1 && !u.calls[0].known, 
       u.calls.empty() ? "none" : u.calls[0].name);

    ok("  and the model is told, not left waiting",
       u.text == "saw the refusal", u.text);

    // A handler that throws is the tool failing, not the loop failing.
    jcode::tool breaks;

    breaks.name = "breaks";
    breaks.run = [](const std::string&) -> std::string {
        throw std::runtime_error("disk on fire");
    };

    std::size_t r2 = 0;

    const jcode::conversation t = jcode::converse(
        { { "user", "go" } }, { breaks },
        [&r2](const std::vector<ai::message>&, const std::string&) -> oa::answer {
            oa::answer a;

            if(r2++ == 0) {
                oa::call c;

                c.name = "breaks";
                c.arguments = "{}";

                a.calls.push_back(c);
            }
            else a.content = "oh well";

            return a;
        });

    ok("a handler that throws does not end the conversation",
       t.why == jcode::ending::answered, jcode::spell(t.why));

    ok("  but is recorded as having failed",
       t.calls.size() == 1 && t.calls[0].failed == "disk on fire",
       t.calls.empty() ? "none" : t.calls[0].failed);
}

/** The declaration the far end is sent. */
static void what_the_tools_look_like_on_the_wire() {
    std::cout << "\nwhat the tool list looks like on the wire:\n";

    jcode::tool t;

    t.name = "read_file";
    t.description = "Read a file";
    t.parameters = R"({"type":"object","properties":{"path":{"type":"string"}}})";

    const std::string j = jcode::declare({ t });

    ok("it is a JSON array of functions",
       j.find("\"type\"") != std::string::npos &&
       j.find("\"function\"") != std::string::npos, j);

    ok("  naming the tool", j.find("read_file") != std::string::npos, j);

    // The schema is the tool author's and has to survive whole.
    ok("  and the schema survives nested",
       j.find("properties") != std::string::npos &&
       j.find("path") != std::string::npos, j);

    ok("no tools is no list at all", jcode::declare({}).empty());
}

/**
 * The tools, and the root they cannot leave.
 *
 * #333. Containment is the load-bearing part: a read that escapes the root is
 * this harness doing the thing #242 says it must not, and "refused" and "read
 * something it should not have" both look like a call that returned -- so each
 * refusal is broken once below and watched to fail.
 *
 * The refusals are **results, not exceptions**: a model that asked for
 * something it may not have is told so and gets another turn, rather than
 * having the conversation end underneath it.
 */
/**
 * Write, build, and tell the model what the compiler said.
 *
 * #341. Every part is scripted -- the model, the write, the build -- so this
 * needs no compiler, no model and no disk, which is the same reason
 * `converse` takes its exchange as a functor.
 */
static void until_the_build_passes() {
    std::cout << "\nwriting, building, and trying again:\n";

    const std::vector<std::string> known{ "util.c" };

    // A model that answers with a file each time; the content changes per
    // attempt so nothing looks stalled.
    std::size_t said = 0;

    const jcode::exchange answers =
        [&said](const std::vector<ai::message>&,
                const std::string&) -> oa::answer {
            oa::answer a;

            a.content = "util.c\n```\nversion " +
                        std::to_string(++said) + "\n```\n";

            return a;
        };

    // A build that fails twice and then passes.
    std::size_t built = 0;

    const jcode::builder eventually = [&built]() -> jcode::built {
        jcode::built b;

        b.passed = ++built >= 3;
        b.output = b.passed ? "exit 0\nall good"
                            : "exit 1\nutil.c:44: undeclared 'test_copy_into'";

        return b;
    };

    std::size_t writes = 0;

    const jcode::writer takes = [&writes](const jcode::reply&) {
        writes++;

        return true;
    };

    const jcode::attempts got =
        jcode::until_built({ { "user", "fix it" } }, {}, answers, takes,
                           eventually, known, 5);

    ok("it stops when the build passes", got.why == jcode::settled::built,
       jcode::spell(got.why));

    ok("  on the third attempt", got.made == 3, std::to_string(got.made));

    ok("  having written each time", writes == 3, std::to_string(writes));

    // **The assertion that matters.**  "fed the failure back" and "went round
    // again without it" look identical from outside, so the turn has to be
    // checked for rather than inferred from the loop having continued.
    std::string saw;

    const jcode::exchange watching =
        [&saw](const std::vector<ai::message>& turns,
               const std::string&) -> oa::answer {
            for(const ai::message& m : turns)
                if(m.content.find("undeclared") != std::string::npos)
                    saw = m.content;

            oa::answer a;

            a.content = "util.c\n```\nsomething " +
                        std::to_string(turns.size()) + "\n```\n";

            return a;
        };

    built = 0;

    jcode::until_built({ { "user", "fix it" } }, {}, watching, takes,
                       eventually, known, 5);

    ok("  and the build output reached the model",
       saw.find("util.c:44") != std::string::npos, saw);

    ok("  as a user turn, not a tool result without a call",
       saw.find("That did not work") != std::string::npos, saw);
}

/** The four ways it stops that are not "the build passed". */
static void the_attempts_know_why_they_stopped() {
    std::cout << "\nthe attempts know why they stopped:\n";

    const std::vector<std::string> known{ "util.c" };

    // As the real one does: false when nothing reached the disk.
    const jcode::writer takes =
        [](const jcode::reply& r) { return !r.edits.empty(); };

    const jcode::builder never = []() -> jcode::built {
        jcode::built b;

        b.output = "exit 1\nstill broken";

        return b;
    };

    // Different content every time, so it is the bound that stops it.
    std::size_t n = 0;

    const jcode::attempts bounded = jcode::until_built(
        { { "user", "go" } }, {},
        [&n](const std::vector<ai::message>&, const std::string&) {
            oa::answer a;

            a.content = "util.c\n```\nv" + std::to_string(++n) + "\n```\n";

            return a;
        },
        takes, never, known, 2);

    ok("a build that never passes runs out of attempts",
       bounded.why == jcode::settled::bounded, jcode::spell(bounded.why));

    ok("  at the bound it was given", bounded.made == 2,
       std::to_string(bounded.made));

    // The same file twice: the next attempt would write the same bytes.
    const jcode::attempts stalled = jcode::until_built(
        { { "user", "go" } }, {},
        [](const std::vector<ai::message>&, const std::string&) {
            oa::answer a;

            a.content = "util.c\n```\nthe same\n```\n";

            return a;
        },
        takes, never, known, 5);

    ok("a model that stops changing anything stops the loop",
       stalled.why == jcode::settled::stalled, jcode::spell(stalled.why));

    ok("  after the repeat, not before it", stalled.made == 2,
       std::to_string(stalled.made));

    // Nothing to write.
    const jcode::attempts empty = jcode::until_built(
        { { "user", "go" } }, {},
        [](const std::vector<ai::message>&, const std::string&) {
            oa::answer a;

            a.content = "I do not think anything needs changing.";

            return a;
        },
        takes, never, known, 5);

    ok("a reply with no files is not an attempt at anything",
       empty.why == jcode::settled::nothing, jcode::spell(empty.why));

    // A write the user declined, or one entirely refused.
    const jcode::attempts declined = jcode::until_built(
        { { "user", "go" } }, {},
        [](const std::vector<ai::message>&, const std::string&) {
            oa::answer a;

            a.content = "util.c\n```\nbody\n```\n";

            return a;
        },
        [](const jcode::reply&) { return false; }, never, known, 5);

    ok("  and neither is a write nobody took",
       declined.why == jcode::settled::nothing, jcode::spell(declined.why));

    // The conversation itself failing is its own ending.
    const jcode::attempts broke = jcode::until_built(
        { { "user", "go" } }, {},
        [](const std::vector<ai::message>&, const std::string&) -> oa::answer {
            throw std::runtime_error("connection refused");
        },
        takes, never, known, 5);

    ok("an exchange that throws ends the attempts too",
       broke.why == jcode::settled::failed, jcode::spell(broke.why));

    ok("  carrying the reason",
       broke.detail.find("connection refused") != std::string::npos,
       broke.detail);
}

static void the_tools_and_their_root() {
    std::cout << "\nthe tools, and the root they cannot leave:\n";

    // A little tree: root/in.txt, and a secret outside it.
    const std::string base = "/tmp/jcode-tools-test";

    ::system(("rm -rf " + base).c_str());
    ::system(("mkdir -p " + base + "/root/sub").c_str());

    {
        std::ofstream f((base + "/root/in.txt").c_str());

        f << "hello from inside\nsecond line\n";
    }

    {
        std::ofstream f((base + "/secret.txt").c_str());

        f << "should never be read\n";
    }

    ::symlink((base + "/secret.txt").c_str(),
              (base + "/root/link.txt").c_str());

    const std::string root = base + "/root";

    const std::vector<jcode::tool> box = jcode::toolbox(root);

    ok("two tools without a build command", box.size() == 2,
       std::to_string(box.size()));

    const jcode::tool* read = 0;
    const jcode::tool* find = 0;

    for(const jcode::tool& t : box) {
        if(t.name == "read_file") read = &t;
        if(t.name == "search") find = &t;
    }

    ok("  read_file and search", read && find);

    if(!read || !find) return;

    ok("a file inside is read",
       read->run(R"({"path":"in.txt"})").find("hello from inside") !=
           std::string::npos,
       read->run(R"({"path":"in.txt"})"));

    // The three ways out, which are one question with one answer.
    ok("  a path climbing out is refused",
       read->run(R"({"path":"../secret.txt"})").find("error:") == 0,
       read->run(R"({"path":"../secret.txt"})"));

    ok("  an absolute path is refused",
       read->run(std::string(R"({"path":")") + base +
                 R"(/secret.txt"})").find("error:") == 0);

    // The one inspection would miss: the name is innocent and the file is not.
    ok("  and a symlink pointing out is refused",
       read->run(R"({"path":"link.txt"})").find("error:") == 0,
       read->run(R"({"path":"link.txt"})"));

    ok("  a file that is not there is refused, not invented",
       read->run(R"({"path":"nope.txt"})").find("error:") == 0);

    ok("  and a call with no path at all",
       read->run("{}").find("error:") == 0);

    // Truncation has to be visible: a silently shortened file is worse than a
    // refusal, because the model believes it read the whole thing.
    {
        std::ofstream f((root + "/big.txt").c_str());

        for(int i = 0; i < 2000; i++) f << "xxxxxxxxxxxxxxxxxxxx\n";
    }

    const std::string cut =
        jcode::toolbox(root, std::string(), 256)[0].run(R"({"path":"big.txt"})");

    ok("a result at the cap says it was cut",
       cut.find("[truncated:") != std::string::npos &&
       cut.size() < 400, std::to_string(cut.size()));

    // search
    ok("search finds a line",
       find->run(R"({"text":"hello from inside"})").find("in.txt") !=
           std::string::npos,
       find->run(R"({"text":"hello from inside"})"));

    ok("  and says so when there is nothing",
       find->run(R"({"text":"absolutely-not-present-anywhere"})") ==
           "no matches");

    ok("  and refuses an empty search",
       find->run("{}").find("error:") == 0);

    // **A model searching code writes code**, and code is full of regex
    // metacharacters. Read as a regular expression, `char*` means "cha and
    // then zero or more r" and matches nothing -- which is what a live run
    // did, silently, for every search worth making.
    {
        std::ofstream f((root + "/sig.c").c_str());

        f << "void reverse(char* s)\n{\n}\n";
    }

    ok("a search containing regex characters is still a string search",
       find->run(R"J({"text":"void reverse(char* s)"})J").find("sig.c") !=
           std::string::npos,
       find->run(R"J({"text":"void reverse(char* s)"})J"));

    ok("  and one that would only match as a regex does not",
       find->run(R"({"text":"cha.* s"})") == "no matches",
       find->run(R"({"text":"cha.* s"})"));

    // The secret is outside the root, so a search must not reach it.
    ok("  and does not reach outside the root",
       find->run(R"({"text":"should never be read"})") == "no matches",
       find->run(R"({"text":"should never be read"})"));

    // build: absent unless named, and it runs the argv it was given.
    ok("no build tool unless one was named",
       jcode::toolbox(root).size() == 2);

    const std::vector<jcode::tool> with =
        jcode::toolbox(root, "echo built-ok");

    ok("  and one when it was", with.size() == 3, std::to_string(with.size()));

    const jcode::tool* run = 0;

    for(const jcode::tool& t : with) if(t.name == "build") run = &t;

    ok("  named build", run != 0);

    if(run) {
        const std::string said = run->run("{}");

        ok("  which runs it and reports the status",
           said.find("exit 0") != std::string::npos &&
           said.find("built-ok") != std::string::npos, said);
    }

    // A build that fails is an answer, not an error.
    const std::vector<jcode::tool> bad = jcode::toolbox(root, "false");

    for(const jcode::tool& t : bad)
        if(t.name == "build")
            ok("  a build that fails reports its status rather than throwing",
               t.run("{}").find("exit 1") != std::string::npos, t.run("{}"));

    // **No shell.**  A command with a shell operator is argv, so the operator
    // is an argument rather than a second command.
    const std::vector<jcode::tool> shell =
        jcode::toolbox(root, "echo one && echo two");

    for(const jcode::tool& t : shell)
        if(t.name == "build")
            ok("  and a shell operator is an argument, not an operator",
               t.run("{}").find("&&") != std::string::npos, t.run("{}"));

    ::system(("rm -rf " + base).c_str());
}

static void the_envelope_a_model_actually_sends() {
    std::cout << "\nthe envelope a model actually sends:\n";

    struct { const char* what; const char* name; const char* wanted;
             const char* says; } cases[] = {
        { "bold",             "**main.cc**", "main.cc", "bold" },
        { "backticks",        "`main.cc`",   "main.cc", "backticks" },
        { "a heading",        "## main.cc",  "main.cc", "heading" },
        { "a trailing colon", "main.cc:",    "main.cc", "colon" },
        { "the prompt's own path prefix", "path/to/main.cc", "main.cc",
          "basename" },
    };

    for(const auto& c : cases) {
        const jcode::reply r = jcode::parse(fence(c.name, "x\n"), KNOWN);

        ok(std::string("  ") + c.what + " is absorbed",
           r.edits.size() == 1 && r.edits[0].name == c.wanted,
           r.edits.empty() ? "no edit" : r.edits[0].name);

        // Absorbed is not the same as hidden.  This is the half of the policy
        // that is easy to skip and is the whole reason leniency is safe.
        const bool said = !r.edits.empty() && !r.edits[0].guesses.empty() &&
            r.edits[0].guesses[0].find(c.says) != std::string::npos;

        ok(std::string("    and logged as a guess"), said,
           r.edits.empty() || r.edits[0].guesses.empty()
               ? "nothing logged" : r.edits[0].guesses[0]);
    }
}

static void a_block_with_no_name() {
    std::cout << "\na block with no filename before it:\n";

    {
        // A model correcting itself: second block, same file, no name.
        const jcode::reply r = jcode::parse(
            fence("main.cc", "first\n") + "\nSorry, better:\n\n```\nsecond\n```\n",
            KNOWN);

        // **One edit, and it is the correction.**  This asserted two until
        // the duplicate collapse landed: both blocks named main.cc, apply()
        // wrote them in order, and the file ended as the second anyway. What
        // the first bought was an announced write that did not survive.
        ok("the correction is the edit, and it is alone",
           r.edits.size() == 1 && r.edits[0].name == "main.cc" &&
           r.edits[0].content == "second\n",
           std::to_string(r.edits.size()) + " edits");

        ok("  having taken the name of the block before it",
           r.edits.size() == 1 && !r.edits[0].guesses.empty() &&
           r.edits[0].guesses[0].find("used the one before it") !=
               std::string::npos,
           r.edits.empty() || r.edits[0].guesses.empty()
               ? "nothing logged" : r.edits[0].guesses[0]);

        // The line before that second fence was "Sorry, better:", which the
        // first version of this took for a filename and wrote an edit to.
        ok("  and did not take the prose line for a filename",
           r.edits.size() == 1 && r.edits[0].name != "Sorry, better",
           r.edits.empty() ? "" : r.edits[0].name);

        ok("  with the one it replaced refused rather than forgotten",
           r.refusals.size() == 1 &&
           r.refusals[0].why.find("superseded") != std::string::npos,
           r.refusals.empty() ? "none" : r.refusals[0].why);
    }

    {
        // Only one file in play: there is nothing else it could mean.
        const std::vector<std::string> one{ "only.cc" };

        const jcode::reply r = jcode::parse("```\nbody\n```\n", one);

        ok("with one file sent, a nameless block is that file",
           r.edits.size() == 1 && r.edits[0].name == "only.cc",
           r.edits.empty() ? "no edit" : r.edits[0].name);
    }

    {
        // Two files and no name: a guess here would be a coin toss, and a
        // coin toss writes the wrong file half the time.
        const jcode::reply r = jcode::parse("```\nbody\n```\n", KNOWN);

        ok("with several sent and no name, it is refused",
           r.edits.empty() && r.refusals.size() == 1,
           std::to_string(r.edits.size()) + " edits, " +
           std::to_string(r.refusals.size()) + " refusals");
    }

    {
        // A paragraph before a fence is prose, not a filename.
        const std::string prose(300, 'w');

        const jcode::reply r = jcode::parse(prose + "\n```\nbody\n```\n", KNOWN);

        ok("a 300-character line before a fence is not a filename",
           r.edits.empty() && r.refusals.size() == 1,
           std::to_string(r.edits.size()) + " edits");
    }
}

static void a_reply_that_stopped() {
    std::cout << "\na reply that stopped inside a block:\n";

    // jserve runs into the context wall and the reply ends mid-file.  What is
    // in hand is the beginning of a file, and writing it would leave a
    // corrupted one where a whole one was -- #256 from the other side.
    const jcode::reply r = jcode::parse(
        "main.cc\n```\nint main() {\n    // and then the context ran out",
        KNOWN);

    ok("nothing is taken from it", r.edits.empty(),
       std::to_string(r.edits.size()) + " edits");

    ok("  it is refused", r.refusals.size() == 1);

    ok("  naming the file it would have been", !r.refusals.empty() &&
       r.refusals[0].name == "main.cc",
       r.refusals.empty() ? "" : r.refusals[0].name);

    ok("  and saying why", !r.refusals.empty() &&
       r.refusals[0].why.find("ended inside") != std::string::npos,
       r.refusals.empty() ? "" : r.refusals[0].why);
}

// ---------------------------------------------------------------- applying

static void applying_it() {
    std::cout << "\nputting it on disk:\n";

    tree t;

    ok("a temp tree", !t.path.empty(), t.path);

    if(t.path.empty()) return;

    t.put("main.cc", "old\n");

    {
        const jcode::reply r = jcode::parse(fence("main.cc", "new\n"), KNOWN);

        const std::vector<jcode::result> got = jcode::apply(r, t.path);

        ok("an edit is written", got.size() == 1 &&
           got[0].what == jcode::outcome::written,
           got.empty() ? "none" : jcode::spell(got[0].what));

        bool found = false;

        ok("  and the file holds it", t.get("main.cc", found) == "new\n" && found);
    }

    {
        // The model returned what it was given.  "applied" would be a lie.
        const jcode::reply r = jcode::parse(fence("main.cc", "new\n"), KNOWN);

        const std::vector<jcode::result> got = jcode::apply(r, t.path);

        ok("an identical file is unchanged, not written",
           got.size() == 1 && got[0].what == jcode::outcome::unchanged,
           got.empty() ? "none" : jcode::spell(got[0].what));
    }

    {
        const jcode::reply r = jcode::parse(fence("fresh.cc", "hello\n"), KNOWN);

        const std::vector<jcode::result> got = jcode::apply(r, t.path);

        ok("a name nothing matched is created, not written",
           got.size() == 1 && got[0].what == jcode::outcome::created,
           got.empty() ? "none" : jcode::spell(got[0].what));
    }

    {
        // Decide everything, write nothing.
        const jcode::reply r = jcode::parse(fence("main.cc", "third\n"), KNOWN);

        const std::vector<jcode::result> got = jcode::apply(r, t.path, true);

        bool found = false;

        ok("a dry run says what it would do", got.size() == 1 &&
           got[0].what == jcode::outcome::written);

        ok("  and does not do it", t.get("main.cc", found) == "new\n");
    }
}

static void what_it_will_not_write() {
    std::cout << "\nwhat it will not write, whatever the model says:\n";

    tree t;

    if(t.path.empty()) { ok("a temp tree", false); return; }

    // **The target is unique to this run**, and that is not tidiness.
    //
    // It was "/tmp/escaped.cc", and when a deliberate break removed the
    // containment check the test wrote it -- after which every later run
    // failed on a file the earlier one had left, including runs of the
    // *fixed* code.  A test that asserts a path does not exist has to own
    // that path, or it is asserting something about the machine.
    const std::string mark = t.path.substr(t.path.find_last_of('/') + 1);

    const std::string away = "/tmp/escaped_" + mark + ".cc";
    const std::string through = "/tmp/through_" + mark + ".cc";

    ::unlink(away.c_str());
    ::unlink(through.c_str());

    const std::string climb = "../escaped_" + mark + ".cc";
    const std::string deeper = "a/../../escaped_" + mark + ".cc";

    struct { std::string what; std::string name; } bad[] = {
        { "a path climbing out",  climb },
        { "a deeper climb",       deeper },
        { "an absolute path",     away },
        { "somebody else's file", "/etc/passwd" },
    };

    for(const auto& b : bad) {
        const jcode::reply r = jcode::parse(fence(b.name, "x\n"), KNOWN);

        const std::vector<jcode::result> got = jcode::apply(r, t.path);

        ok("  " + b.what + " is refused",
           got.size() == 1 && got[0].what == jcode::outcome::refused,
           got.empty() ? "none" : jcode::spell(got[0].what));

        ok("    and it is not on disk", ::access(away.c_str(), F_OK) != 0,
           away);
    }

    // A symlink out is the same question, and the one a text check loses --
    // the name says "inside" and the filesystem says otherwise.  #262 was a
    // check that read the text where the filesystem had the answer.
    const std::string out = t.path + "/away";

    if(::symlink("/tmp", out.c_str()) == 0) {
        const jcode::reply r = jcode::parse(
            fence("away/through_" + mark + ".cc", "x\n"), KNOWN);

        const std::vector<jcode::result> got = jcode::apply(r, t.path);

        ok("  a symlink pointing out is refused",
           got.size() == 1 && got[0].what == jcode::outcome::refused,
           got.empty() ? "none" : got[0].why);

        ok("    and nothing was written through it",
           ::access(through.c_str(), F_OK) != 0, through);
    }

    ::unlink(away.c_str());
    ::unlink(through.c_str());
}

// ---------------------------------------------------------------- the prompt

static void the_estimate_errs_high() {
    std::cout << "\nestimating tokens from bytes:\n";

    // The measured worst case: TinyLlama's vocabulary on jlib's densest
    // header.  Everything else is looser, so everything else is over-counted,
    // which is the direction that does not lose the head of a conversation.
    ok("the divisor is the worst ratio measured, not the mean",
       jcode::bytes_per_token() <= 2.80 + 1e-9 &&
       jcode::bytes_per_token() > 2.0,
       std::to_string(jcode::bytes_per_token()));

    const std::string k(2800, 'x');

    ok("2800 bytes is about a thousand tokens",
       jcode::estimate_tokens(k) >= 1000 && jcode::estimate_tokens(k) <= 1002,
       std::to_string(jcode::estimate_tokens(k)));

    ok("  and it is an over-estimate against every vocabulary measured",
       double(jcode::estimate_tokens(k)) * 3.12 >= 2800.0,
       std::to_string(jcode::estimate_tokens(k)));

    ok("nothing costs nothing", jcode::estimate_tokens("") == 0);

    ok("  and a fragment of a token still costs one",
       jcode::estimate_tokens("a") == 1,
       std::to_string(jcode::estimate_tokens("a")));
}

static void the_prompt_says_what_the_parser_reads() {
    std::cout << "\nthe prompt and the parser are one contract:\n";

    const std::string p = jcode::system_prompt();

    // Every leniency parse() carries is a rule stated here.  If the prompt
    // stops saying one, the parser is absorbing a failure nobody asked the
    // model to avoid.
    struct { const char* what; const char* says; } rules[] = {
        { "the filename alone on a line", "line of its own" },
        { "no bold",                      "no bold" },
        { "no backticks",                 "no backticks" },
        { "no heading",                   "no heading" },
        { "no trailing colon",            "trailing colon" },
        { "three backticks",              "three backticks" },
        { "what is left out is deleted",  "deleted" },
    };

    for(const auto& r : rules)
        ok(std::string("  it asks for: ") + r.what,
           p.find(r.says) != std::string::npos);
}

static void laying_out_a_request() {
    std::cout << "\nlaying out a request:\n";

    std::vector<jcode::source> files{
        { "small.cc", std::string(280, 'a') },
        { "medium.cc", std::string(2800, 'b') },
        { "huge.cc", std::string(28000, 'c') },
    };

    {
        const jcode::plan p = jcode::lay_out("make it faster", files, 100000);

        ok("with room for everything, everything goes", p.sent.size() == 3,
           std::to_string(p.sent.size()) + " sent");

        ok("  nothing dropped", p.dropped.empty());

        ok("  a system turn and a user turn", p.turns.size() == 2 &&
           p.turns[0].first == "system" && p.turns[1].first == "user",
           std::to_string(p.turns.size()));

        ok("  the request is in the user turn",
           p.turns[1].second.find("make it faster") != std::string::npos);

        ok("  and so are the files",
           p.turns[1].second.find("medium.cc") != std::string::npos);
    }

    {
        const jcode::plan p = jcode::lay_out("make it faster", files, 1500);

        ok("a file that does not fit is left out", p.sent.size() == 2 &&
           p.dropped.size() == 1,
           std::to_string(p.sent.size()) + " sent, " +
           std::to_string(p.dropped.size()) + " dropped");

        ok("  it is the one that did not fit", !p.dropped.empty() &&
           p.dropped[0].find("huge.cc") != std::string::npos,
           p.dropped.empty() ? "" : p.dropped[0]);

        // Whole files, never truncated: half a file in a prompt is worse than
        // none, because a model completes it rather than noticing.
        ok("  and nothing of it is in the prompt",
           p.turns[1].second.find("ccc") == std::string::npos);

        ok("  the drop says what it cost and what was left",
           !p.dropped.empty() &&
           p.dropped[0].find("tokens") != std::string::npos &&
           p.dropped[0].find("already used") != std::string::npos,
           p.dropped.empty() ? "" : p.dropped[0]);
    }

    {
        // The request is never dropped.  Refusing it here would be this code
        // deciding what the model may read.
        const jcode::plan p = jcode::lay_out("tiny question", files, 1);

        ok("a budget of one still carries the request",
           p.turns.size() == 2 &&
           p.turns[1].second.find("tiny question") != std::string::npos);

        ok("  with every file dropped", p.sent.empty() && p.dropped.size() == 3,
           std::to_string(p.dropped.size()) + " dropped");
    }
}

static void the_estimate_reports_its_own_error() {
    std::cout << "\nwhat the server says it really cost:\n";

    const std::string over = jcode::estimate_drift(1200, 1000);

    ok("an over-estimate says so", over.find("over by 200") != std::string::npos,
       over);

    ok("  as a percentage too", over.find("20%") != std::string::npos, over);

    const std::string under = jcode::estimate_drift(900, 1000);

    ok("an under-estimate says so too",
       under.find("under by 100") != std::string::npos, under);

    ok("an exact one says that",
       jcode::estimate_drift(500, 500).find("exact") != std::string::npos,
       jcode::estimate_drift(500, 500));

    ok("and a reply with no usage block says nothing at all",
       jcode::estimate_drift(500, 0).empty());
}

int main() {
    std::cout << std::unitbuf;

    the_format_as_asked_for();
    a_file_given_twice_is_written_once();
    a_tool_call_in_the_reply();
    until_the_build_passes();
    the_attempts_know_why_they_stopped();
    the_tools_and_their_root();
    the_agent_loop();
    the_loop_knows_why_it_stopped();
    what_the_tools_look_like_on_the_wire();
    the_envelope_a_model_actually_sends();
    a_block_with_no_name();
    a_reply_that_stopped();
    applying_it();
    what_it_will_not_write();
    the_estimate_errs_high();
    the_prompt_says_what_the_parser_reads();
    laying_out_a_request();
    the_estimate_reports_its_own_error();

    // What a green run does not establish.
    //
    // **Not that a model sends any of this.** Every reply here was written by
    // hand from aider's own parser and prompt, which is a record of what
    // models did to *it*. The first real one is jcode's first conversation.
    //
    // Not the fence collision. A file whose own content contains a fence line
    // cannot survive this format -- the block ends early and the rest reads as
    // prose -- and nothing here asserts what happens, because what happens is
    // "something wrong" and the honest fix is at the sending end.
    //
    // Not concurrency, not encoding, and not permissions: a file that cannot
    // be opened is refused, and that is the only failure of the write path
    // covered.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
