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
#include <sstream>
#include <string>
#include <vector>

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

namespace jcode = jlib::apps::jcode;

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

        ok("the second block takes the name of the first",
           r.edits.size() == 2 && r.edits[1].name == "main.cc",
           std::to_string(r.edits.size()) + " edits");

        ok("  and says it did", r.edits.size() == 2 &&
           !r.edits[1].guesses.empty() &&
           r.edits[1].guesses[0].find("used the one before it") !=
               std::string::npos,
           r.edits.size() < 2 || r.edits[1].guesses.empty()
               ? "nothing logged" : r.edits[1].guesses[0]);

        // The line before that second fence was "Sorry, better:", which the
        // first version of this took for a filename and wrote an edit to.
        ok("  and did not take the prose line for a filename",
           r.edits.size() == 2 && r.edits[1].name != "Sorry, better",
           r.edits.size() < 2 ? "" : r.edits[1].name);
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
    a_tool_call_in_the_reply();
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
