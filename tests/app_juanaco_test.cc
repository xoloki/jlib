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

// juanaco, driven through a pty -- #446, #447 and #448, phases 2 to 4 of #444.
//
// This is what #445 was built for, and the reason it was built first: jlib's
// other curses program has no test at all, and #247 records that its harness
// lives outside the repo. There was never going to be a good moment to add
// one retroactively.
//
// ## The three sorts of section here, and what each is for
//
// **The skeleton** needs no server: the layout, the slash commands, and
// where the cursor rests are the program's own state.
//
// **End to end** points juanaco at the scripted endpoint from `scripted.hh`
// -- a real socket and the real wire format, with the reply decided by this
// file -- and asserts that what it said reaches the screen and that the edit
// reaches the disk. No model, no GGUF, no network.
//
// **A server that never answers** is the one that needed a feature added to
// the harness to write at all. It is how the status line's state, the
// refusal of a second request, and leaving without waiting out a ten-minute
// timeout are checked -- none of which can be observed against an endpoint
// that replies in a millisecond.
//
// **A server that answers slowly** is the same argument one step on, and it
// is what #448 needed. A stream that arrives instantly cannot distinguish a
// client that renders tokens as they come from one that waits for all of
// them: the reply is on screen in one repaint either way. So the scripted
// endpoint delays its chunks, and the assertions are about what is on screen
// *before* the reply has finished.

#include "pty.hh"
#include "scripted.hh"
#include "vt.hh"

#include <jlib/sys/server.hh>
#include <jlib/sys/sys.hh>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace oa = jlib::ai::openai;
namespace sys = jlib::sys;
namespace tests = jlib::tests;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    std::cout << (good ? "  ok   " : "  FAIL ") << what;

    // **Only when it failed.** A detail printed beside "ok" reads as a
    // failure report, which is how a green run gets scrolled past.
    if(!good && !detail.empty()) std::cout << ": " << detail;

    std::cout << "\n";

    if(!good) failures++;
}

/**
 * Where the program is, from where the test runs.
 *
 * The libtool wrapper rather than `.libs/juanaco`: it is a shell script that
 * sets the library path the uninstalled shared libraries need, and running
 * the real binary directly would need the test to reproduce that.
 */
static std::string juanaco() {
    if(const char* e = std::getenv("JUANACO")) return e;

    return "../jlib/apps/juanaco";
}

/** jcode, beside juanaco in the build tree, for the sections that compare. */
static std::string jcode_path() {
    if(const char* e = std::getenv("JCODE")) return e;

    return "../jlib/apps/jcode";
}

static const int ROWS = 30;
static const int COLS = 110;

// Which rows the transcript occupies: header, rule, the transcript, a rule,
// the prompt, the status line. With 30 rows that is 2..26, the prompt at 28
// and the status line at 29.
static const int FIRST_ROW = 2;
static const int LAST_ROW = ROWS - 4;
static const int PROMPT_ROW = ROWS - 2;
static const int STATUS_ROW = ROWS - 1;

/**
 * The screen, rebuilt from everything the program has written.
 *
 * **Assert on this, not on the stream.** `pty::expect` is still a
 * synchronisation primitive, but what is asserted is the reconstructed
 * screen, because the two are not the same question. A pad blitted and then
 * overwritten paints its text and shows nothing, and that is exactly the bug
 * this test used to pass: every slash command's answer was in the stream and
 * none of it was on screen. See the note at the top of `vt.hh`.
 */
static tests::vt shown(tests::pty& p) {
    tests::vt v(ROWS, COLS);

    v.feed(p.seen());

    return v;
}

/**
 * Poll the screen until it satisfies `pred`.
 *
 * **Polling the screen, not waiting for a string.** The program draws the
 * prompt, so it echoes whatever the test types -- and `expect` on text the
 * test just sent matches that echo and returns before the command has been
 * processed at all. Two assertions passed and then failed for exactly that
 * reason once the screen was being checked.
 */
template<typename Pred>
static bool until(tests::pty& p, Pred pred, double seconds) {
    for(int i = 0; i < int(seconds * 10); i++) {
        if(pred(shown(p))) return true;

        p.settle(0.1);
    }

    return pred(shown(p));
}

/**
 * Whether `text` is on a transcript row rather than in the prompt.
 *
 * **The prompt and a user's entry look the same on screen**: both are drawn
 * with "> ", which is right for a user -- the line appears as it was typed --
 * and means a text match alone cannot tell "being typed" from "submitted". So
 * anything about a user's line has to say *where*.
 */
static bool in_transcript(const tests::vt& v, const std::string& text) {
    const int r = v.find(text);

    return r >= FIRST_ROW && r <= LAST_ROW;
}

static double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t)
        .count();
}

/** What a write question has on its row and nothing else does. */
static const std::string DIALOG = "[y] yes";

/** The write question on the prompt row, or empty when there is none. */
static std::string question_on(const tests::vt& v, int row = PROMPT_ROW) {
    const std::string r = v.row(row);

    return r.find(DIALOG) == std::string::npos ? std::string() : r;
}

/**
 * Answer every write question with `key` until the worker is idle, and say
 * how many there were.
 *
 * **Every question is seen, by construction.** The worker blocks on each one
 * until a key arrives, so none can be answered behind this loop's back --
 * which is what makes the count a measurement rather than a sample. That
 * count is the "one dialog, not two" assertion #449 says has teeth.
 *
 * It waits for each question to *leave* before looking for the next, so a
 * question still on screen because its answer has not been read yet is not
 * answered twice -- which would be the double-tap the program itself guards
 * against, committed by the test.
 */
static int answer_every(tests::pty& p, const std::string& key,
                        std::vector<std::string>* asked = 0)
{
    // Started, first: straight after Return the status line can still say
    // idle, and reading that as "finished" would end this before it began.
    until(p, [](const tests::vt& v) {
        return !question_on(v).empty() ||
               v.row(STATUS_ROW).find("idle") == std::string::npos;
    }, 10.0);

    int n = 0;

    for(;;) {
        std::string q;

        until(p, [&q](const tests::vt& v) {
            q = question_on(v);

            return !q.empty() ||
                   v.row(STATUS_ROW).find("idle") != std::string::npos;
        }, 30.0);

        if(q.empty()) return n;

        n++;

        if(asked) asked->push_back(q);

        p.send(key);

        until(p, [&q](const tests::vt& v) { return question_on(v) != q; },
              10.0);
    }
}

/** Write a file and return what an edit to it produced, for comparison. */
static std::string hunks_reply(const std::string& name) {
    return name + "\n"
           "<<<<<<< SEARCH\n"
           "    return 1;\n"
           "=======\n"
           "    return 0;\n"
           ">>>>>>> REPLACE\n"
           "\n" + name + "\n"
           "<<<<<<< SEARCH\n"
           "// first\n"
           "=======\n"
           "// first, and changed\n"
           ">>>>>>> REPLACE\n";
}

int main() {
    std::cout << std::unitbuf;

    const std::string prog = juanaco();
    const std::string jcode = jcode_path();

    std::cout << "juanaco's skeleton, through a pty:\n";

    {
        // **Pointed at a port nothing can be listening on.** A typed prompt
        // is a request now, and left on the default URL this section would
        // either hang on somebody's jserve or pass differently depending on
        // whether one happened to be running. Port 1 needs root to bind, so
        // a refusal is the only thing that can happen -- which is itself
        // worth one assertion below.
        tests::pty p({ prog, "--url", "http://127.0.0.1:1/v1",
                       "--task", "fix the off-by-one" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        ok("  it starts and says what it is", shown(p).shows("juanaco"));

        // **A list that cannot be had is said, not waited on** (#450). It is
        // asked for at startup on a relay of its own, so a dead URL is known
        // before anything is typed -- and the ceiling stays "?", not a number
        // borrowed from somewhere else.
        until(p, [](const tests::vt& v) {
            return v.shows("could not list the models");
        }, 10.0);

        ok("  a server that cannot be listed is reported at startup",
           shown(p).shows("could not list the models"), shown(p).dump());

        ok("  and the ceiling is not guessed",
           shown(p).row(STATUS_ROW).find("ctx 0/?") != std::string::npos,
           "[" + shown(p).row(STATUS_ROW) + "]");

        ok("  the task is in the header",
           shown(p).find("fix the off-by-one") == 0,
           "on row " + std::to_string(shown(p).find("fix the off-by-one")));

        // `whole` by default since #447 measured why; see state::edits.
        ok("  and the mode", shown(p).row(0).find("whole") != std::string::npos,
           shown(p).row(0));

        // Ctrl-J adds a line rather than submitting. On a screen this is
        // simply two rows: the entry's first line carries "> " and its
        // continuation does not. Two submissions would put "> " on both.
        p.send("first line\x0asecond line\r");

        until(p, [](const tests::vt& v) {
            return in_transcript(v, "> first line");
        }, 10.0);

        {
            const tests::vt v = shown(p);

            const int a = v.find("> first line");
            const int b = v.find("second line");

            ok("  Ctrl-J continued the prompt rather than sending it",
               a >= 0 && b == a + 1 &&
                   v.row(b).find("> ") == std::string::npos,
               "rows " + std::to_string(a) + " and " + std::to_string(b) +
                   ", second reads [" + v.row(b) + "]");
        }

        // A refused connection has to arrive as a line in the transcript.
        // The failure mode it rules out is the one that matters most for a
        // curses program: a worker that threw and a UI that went on looking
        // as though it were still thinking.
        until(p, [](const tests::vt& v) {
            return v.shows("the conversation failed");
        }, 20.0);

        {
            const tests::vt v = shown(p);

            ok("  a refused endpoint is reported rather than swallowed",
               v.shows("the conversation failed"), v.dump());

            ok("  and the status line goes back to idle",
               v.row(STATUS_ROW).find("idle") != std::string::npos,
               "[" + v.row(STATUS_ROW) + "]");
        }

        // **Away from the default, so the transition is a real one.** This
        // asked for `whole` until #447 made `whole` the default, at which
        // point it proved only that the header had not changed.
        p.send("/mode hunks\r");

        until(p, [](const tests::vt& v) { return v.shows("-- edits hunks"); },
              10.0);

        {
            const tests::vt v = shown(p);

            ok("  /mode hunks is acknowledged", v.shows("edits hunks"));

            ok("  and the header follows it",
               v.row(0).find("hunks") != std::string::npos, v.row(0));
        }

        p.send("/mode build on\r");

        until(p, [](const tests::vt& v) { return v.shows("-- build on"); }, 10.0);

        {
            const tests::vt v = shown(p);

            ok("  build can be turned on, and the header says so",
               v.row(0).find("build") != std::string::npos, v.row(0));

            // A switch with nothing behind it has to say so at the moment it
            // is thrown, not one request later when nothing ran.
            ok("  and with no --build it says there is nothing to run",
               v.shows("nothing to run"));
        }

        // The escape hatch: a coding harness meets a leading slash the first
        // time anyone pastes an absolute path.
        p.send("//usr/local/lib is where it looks\r");

        until(p, [](const tests::vt& v) {
            return in_transcript(v, "> /usr/local/lib is where it looks");
        }, 10.0);

        {
            const tests::vt v = shown(p);

            ok("  // sends a prompt whose first character is a slash",
               in_transcript(v, "> /usr/local/lib is where it looks"),
               "found on row " +
                   std::to_string(v.find("> /usr/local/lib is where it looks")));

            ok("  and it was not taken for a command",
               !v.shows("no such command: /usr"));
        }

        p.send("/nosuchthing\r");

        until(p, [](const tests::vt& v) {
            return v.shows("no such command: /nosuchthing");
        }, 10.0);

        ok("  an unknown command is reported",
           shown(p).shows("no such command: /nosuchthing"));

        p.send("/help\r");

        until(p, [](const tests::vt& v) { return v.shows("/files"); }, 10.0);

        ok("  /help lists the commands", shown(p).shows("/files"));

        // Where the cursor rests is the one thing on screen that is not text,
        // and painting the status line after placing it parked it one row low.
        p.settle(0.5);

        {
            const tests::vt v = shown(p);

            ok("  the cursor is left in the prompt, not on the status line",
               v.cursor_row() == PROMPT_ROW,
               "cursor on row " + std::to_string(v.cursor_row()) +
                   (v.cursor_row() == STATUS_ROW ? " -- the status line" : ""));
        }

        p.send("/quit\r");

        const int status = p.wait_for_exit(10.0);

        ok("  /quit leaves", status == 0, std::to_string(status));
    }

    // ------------------------------------------------- with somewhere to send

    tests::script sc;

    sys::server server(0,
                       [&sc](sys::socketstream& s, const sys::peer&) {
                           tests::serve(s, sc);
                       },
                       "127.0.0.1");

    server.on_error([](const std::exception&, const sys::peer&) {});

    std::thread th([&server]{ server.run(); });

    const std::string url = "http://127.0.0.1:" +
                            std::to_string(server.port()) + "/v1";

    {
        std::cout << "\na reply from a scripted endpoint, end to end:\n";

        tests::tree tr;

        tr.put("hello.cc", "int main() { return 1; }\n");

        sc.content = tests::listing("hello.cc", "int main() { return 0; }\n");

        tests::pty p({ prog, "--url", url, "--root", tr.path, "--yes",
                       "--task", "make it succeed", "hello.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        // The scripted reply is in whole-file format, so ask for that one --
        // `parse` reads either regardless of what was requested, and a test
        // that relied on the leniency would be asserting the wrong thing.
        p.send("/mode whole\r");

        until(p, [](const tests::vt& v) { return v.shows("-- edits whole"); },
              10.0);

        p.send("make it return zero\r");

        until(p, [](const tests::vt& v) {
            return v.shows("written hello.cc");
        }, 20.0);

        {
            const tests::vt v = shown(p);

            ok("  the reply reaches the transcript", v.shows("Here you go."),
               v.dump());

            ok("  the write is announced", v.shows("** written hello.cc"),
               v.dump());

            ok("  and the edit is on disk",
               tr.get("hello.cc") == "int main() { return 0; }\n",
               "[" + tr.get("hello.cc") + "]");

            ok("  the request carried the file",
               sc.last_body.find("int main() { return 1; }") !=
                   std::string::npos);

            // **The assertion #447 wrote to fail here, turned round.**
            // `request::str` adds the key only when streaming, so it is the
            // request body that says which path was taken -- and this is now
            // the streaming one, with `--no-stream` tested below.
            ok("  and asked for it as a stream",
               sc.last_body.find("\"stream\"") != std::string::npos,
               sc.last_body.substr(0, 200));

            ok("  the layout was reported", v.shows("1 of 1 file(s)"), v.dump());

            // **The drift line, on the streaming path, since #450.** It was
            // asserted *absent* here: a chunk had no usage block, so the
            // estimate could only be checked with --no-stream. The usage
            // chunk `include_usage` asks for is what turned that round.
            ok("  and what the token guess was worth, from the usage chunk",
               v.shows("the server counted"), v.dump());

            ok("  which it asked for", sc.include_usage,
               sc.last_body.substr(0, 200));
        }

        // **Counted on the streaming path now** (#450). The scripted endpoint
        // reports 20 completion tokens a request, so the number is one this
        // file chose rather than one read back from what is being tested.
        p.send("/stats\r");

        until(p, [](const tests::vt& v) { return v.shows("tokens in,"); }, 10.0);

        {
            const tests::vt v = shown(p);

            ok("  /stats counts the tokens a stream reported",
               v.shows(" 20 out, across 1 request(s)"), v.dump());

            ok("  and the context against the model's ceiling",
               v.shows(" of 4096 ("), v.dump());

            ok("  and the status line is back to idle",
               v.row(STATUS_ROW).find("idle") != std::string::npos,
               "[" + v.row(STATUS_ROW) + "]");
        }

        // **The loop survived the request.** A worker that left the terminal
        // or the input state wrong would show up here and nowhere else.
        p.send("/mode hunks\r");

        until(p, [](const tests::vt& v) { return v.shows("-- edits hunks"); },
              10.0);

        ok("  and the prompt still works afterwards",
           shown(p).shows("-- edits hunks"));

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);
    }

    {
        std::cout << "\nwithout --yes, every write is a question:\n";

        tests::tree tr;

        tr.put("hello.cc", "original\n");

        sc.content = tests::listing("hello.cc", "changed\n");

        // Slow, so there is a stretch in which to type a draft the dialog
        // must not destroy.
        sc.chunk_delay = 0.15;

        tests::pty p({ prog, "--url", url, "--root", tr.path,
                       "hello.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        ok("  it says up front that writes are asked about",
           shown(p).shows("every write is asked about first"), shown(p).dump());

        p.send("change it\r");

        until(p, [](const tests::vt& v) {
            return v.row(STATUS_ROW).find("streaming") != std::string::npos;
        }, 20.0);

        // A draft, typed while the reply arrives and not submitted.
        p.send("my next thought");

        const bool asked = until(p, [](const tests::vt& v) {
            return !question_on(v).empty();
        }, 30.0);

        {
            const tests::vt v = shown(p);

            ok("  a question appears", asked, v.dump());

            // **With what it touches** (#464) -- for a one-line file replaced
            // whole, its size -- which is what makes the question a check:
            // `write openai.cc?` was the whole of it when a reply was about to
            // turn 1074 lines into 22.
            ok("  saying what it would touch",
               question_on(v).find("write hello.cc? 1 to 1 lines") !=
                   std::string::npos,
               "[" + v.row(PROMPT_ROW) + "]");

            ok("  naming the file and the keys",
               question_on(v).find("write hello.cc?") != std::string::npos,
               "[" + v.row(PROMPT_ROW) + "]");

            // **The worker is blocked on you, and must say so.** `idle`
            // would read as finished, and `streaming` -- what this said
            // until a live run -- as busy.
            ok("  and the status line says it is asking",
               v.row(STATUS_ROW).find("asking") != std::string::npos,
               "[" + v.row(STATUS_ROW) + "]");

            ok("  nothing is written while the question is up",
               tr.get("hello.cc") == "original\n",
               "[" + tr.get("hello.cc") + "]");
        }

        // **A letter that is not an answer does nothing.** It must not land
        // in the hidden draft either, which is what the next assertion
        // would show.
        p.send("q");
        p.settle(0.5);

        ok("  a key that is not y, n or Escape is not an answer",
           !question_on(shown(p)).empty(), "[" + shown(p).row(PROMPT_ROW) + "]");

        p.send("n");

        until(p, [](const tests::vt& v) {
            return v.row(STATUS_ROW).find("idle") != std::string::npos;
        }, 20.0);

        {
            const tests::vt v = shown(p);

            ok("  no leaves the file alone", tr.get("hello.cc") == "original\n",
               "[" + tr.get("hello.cc") + "]");

            ok("  and says so", v.shows("left hello.cc alone"), v.dump());

            ok("  and the answer is on the record",
               v.shows("asked: write hello.cc? 1 to 1 lines no"), v.dump());

            // **The dialog borrows the row; it does not clear it.** The
            // draft comes back exactly, and without the `q` that was not an
            // answer.
            ok("  the draft typed before the question comes back intact",
               v.row(PROMPT_ROW).find("> my next thought") != std::string::npos &&
                   v.row(PROMPT_ROW).find("thoughtq") == std::string::npos,
               "[" + v.row(PROMPT_ROW) + "]");
        }

        sc.chunk_delay = 0;

        p.send("\x15/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);
    }

    {
        std::cout << "\ntwo edits, and the dry run refuses one:\n";

        tests::tree tr;

        tr.put("hello.cc", "original\n");

        const std::string away = "/tmp/juanaco_escape_" +
                                 tr.path.substr(tr.path.find_last_of('/') + 1) +
                                 ".cc";

        ::unlink(away.c_str());

        // **The refused one first**, so that asking before classifying would
        // put it in front of the user before anything else.
        sc.content = tests::listing(away, "escaped\n") +
                     tests::listing("hello.cc", "accepted\n");

        tests::pty p({ prog, "--url", url, "--root", tr.path,
                       "hello.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        p.send("change them\r");

        std::vector<std::string> questions;

        const int n = answer_every(p, "y", &questions);

        const tests::vt v = shown(p);

        // **The assertion with teeth.** Asking first and refusing afterwards
        // writes exactly the same bytes, which is what makes it the easy
        // mistake -- so the bytes cannot be what this checks. The count can.
        ok("  exactly one question, not two", n == 1,
           std::to_string(n) + " asked" +
               (questions.empty() ? std::string() : ", first [" + questions[0] + "]"));

        ok("  and it was about the file that could be written",
           !questions.empty() &&
               questions[0].find("hello.cc") != std::string::npos &&
               questions[0].find("escape") == std::string::npos,
           questions.empty() ? std::string("none") : questions[0]);

        ok("  the refusal is a line with its reason, not a question",
           v.shows("refused " + away + ":"), v.dump());

        ok("  yes writes the one that could be written",
           tr.get("hello.cc") == "accepted\n", "[" + tr.get("hello.cc") + "]");

        ok("  and nothing reached the refused path",
           ::access(away.c_str(), F_OK) != 0, away);

        ::unlink(away.c_str());

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);
    }

    {
        std::cout << "\nyes, then Escape:\n";

        tests::tree tr;

        tr.put("a.cc", "a\n");
        tr.put("b.cc", "b\n");

        sc.content = tests::listing("a.cc", "a changed\n") +
                     tests::listing("b.cc", "b changed\n");

        // **A build that always fails**, which is what makes a second attempt
        // the thing that would happen next. With no build, the first write
        // counts as success and the attempts end whether or not Escape
        // stopped the worker -- so the request count below could not fail.
        tests::pty p({ prog, "--url", url, "--root", tr.path,
                       "--build", "false", "a.cc", "b.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        p.send("/mode build on\r");

        until(p, [](const tests::vt& v) { return v.shows("-- build on"); }, 10.0);

        const int before = sc.requests.load();

        p.send("change both\r");

        until(p, [](const tests::vt& v) { return !question_on(v).empty(); },
              30.0);

        const std::string first = question_on(shown(p));

        p.send("y");

        until(p, [&first](const tests::vt& v) {
            const std::string q = question_on(v);

            return !q.empty() && q != first;
        }, 10.0);

        const std::string second = question_on(shown(p));

        ok("  the second file is asked about in turn",
           second.find("b.cc") != std::string::npos, "[" + second + "]");

        p.send("\x1b");

        until(p, [](const tests::vt& v) {
            return v.row(STATUS_ROW).find("idle") != std::string::npos;
        }, 20.0);

        {
            const tests::vt v = shown(p);

            ok("  the one agreed to is written", tr.get("a.cc") == "a changed\n",
               "[" + tr.get("a.cc") + "]");

            ok("  the one stopped at is not", tr.get("b.cc") == "b\n",
               "[" + tr.get("b.cc") + "]");

            ok("  and it says the asking was stopped",
               v.shows("stopped at the confirm"), v.dump());

            // Stop is not no: the attempt is over, not merely this file. An
            // edit agreed to before the Escape would otherwise be built and
            // sent round again, and the person who pressed Escape would be
            // asked about the next attempt's edits.
            // A difference, not a total: the endpoint has served every
            // section before this one, so "more than none" is true of any
            // run at all and could not fail -- which is what this read
            // until it was looked at again before it was ever run.
            ok("  and no second request went out",
               sc.requests.load() - before == 1,
               std::to_string(sc.requests.load() - before) + " requests");
        }

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);
    }

    {
        std::cout << "\nno is not stop:\n";

        tests::tree tr;

        tr.put("a.cc", "a\n");
        tr.put("b.cc", "b\n");

        sc.content = tests::listing("a.cc", "a changed\n") +
                     tests::listing("b.cc", "b changed\n");

        tests::pty p({ prog, "--url", url, "--root", tr.path,
                       "a.cc", "b.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        p.send("change both\r");

        // n to the first, y to the second: a "no" that silently meant "stop"
        // would never ask the second question at all.
        std::vector<std::string> questions;

        until(p, [](const tests::vt& v) { return !question_on(v).empty(); },
              30.0);

        questions.push_back(question_on(shown(p)));

        p.send("n");

        const bool second = until(p, [&questions](const tests::vt& v) {
            const std::string q = question_on(v);

            return !q.empty() && q != questions[0];
        }, 10.0);

        ok("  a no to the first file still asks about the second", second,
           shown(p).dump());

        p.send("y");

        until(p, [](const tests::vt& v) {
            return v.row(STATUS_ROW).find("idle") != std::string::npos;
        }, 20.0);

        ok("  the first is left alone", tr.get("a.cc") == "a\n",
           "[" + tr.get("a.cc") + "]");

        ok("  and the second is written", tr.get("b.cc") == "b changed\n",
           "[" + tr.get("b.cc") + "]");

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);
    }

    {
        std::cout << "\ny pressed twice:\n";

        tests::tree tr;

        tr.put("a.cc", "a\n");
        tr.put("b.cc", "b\n");

        sc.content = tests::listing("a.cc", "a changed\n") +
                     tests::listing("b.cc", "b changed\n");

        tests::pty p({ prog, "--url", url, "--root", tr.path,
                       "a.cc", "b.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        p.send("change both\r");

        until(p, [](const tests::vt& v) { return !question_on(v).empty(); },
              30.0);

        const std::string first = question_on(shown(p));

        // **A double tap.** Both bytes go down in one write, so the second
        // is already waiting when the first is read -- and the next question
        // appears microseconds later. Without the flushinp() after an answer
        // it lands somewhere: on the second question, unseen, or in the
        // prompt. Both are checked.
        p.send("yy");

        until(p, [&first](const tests::vt& v) {
            const std::string q = question_on(v);

            return !q.empty() && q != first;
        }, 10.0);

        p.settle(0.5);

        const std::string second = question_on(shown(p));

        ok("  the second press did not answer the second question",
           second.find("b.cc") != std::string::npos, "[" + second + "]");

        p.send("n");

        until(p, [](const tests::vt& v) {
            return v.row(STATUS_ROW).find("idle") != std::string::npos;
        }, 20.0);

        {
            const tests::vt v = shown(p);

            // vt::row trims trailing spaces, so an empty prompt reads ">".
            ok("  nor did it land in the prompt", v.row(PROMPT_ROW) == ">",
               "[" + v.row(PROMPT_ROW) + "]");

            ok("  so b.cc is exactly as it was", tr.get("b.cc") == "b\n",
               "[" + tr.get("b.cc") + "]");

            ok("  and a.cc was written", tr.get("a.cc") == "a changed\n",
               "[" + tr.get("a.cc") + "]");
        }

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);
    }

    {
        std::cout << "\nthe window resized mid-question:\n";

        tests::tree tr;

        tr.put("hello.cc", "original\n");

        sc.content = tests::listing("hello.cc", "resized\n");

        tests::pty p({ prog, "--url", url, "--root", tr.path,
                       "hello.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        p.send("change it\r");

        until(p, [](const tests::vt& v) { return !question_on(v).empty(); },
              30.0);

        // Bigger, so nothing drawn before the resize is clipped by the model
        // the screen is rebuilt into. The prompt row moves with the bottom.
        const int R2 = ROWS + 6;
        const int C2 = COLS + 10;

        ok("  the harness can resize the terminal", p.resize(R2, C2));

        const auto bigger = [&p, R2, C2]() {
            tests::vt v(R2, C2);

            v.feed(p.seen());

            return v;
        };

        bool still = false;

        for(int i = 0; i < 50 && !still; i++) {
            p.settle(0.1);

            still = !question_on(bigger(), R2 - 2).empty();
        }

        ok("  the question is still on screen after it, on the new prompt row",
           still, "[" + bigger().row(R2 - 2) + "]");

        p.send("y");

        until(p, [](const tests::vt& v) { return v.shows("written hello.cc"); },
              20.0);

        ok("  and answering it still works", tr.get("hello.cc") == "resized\n",
           "[" + tr.get("hello.cc") + "]");

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);
    }

    {
        std::cout << "\nCtrl-C while the worker is blocked on a question:\n";

        tests::tree tr;

        tr.put("hello.cc", "original\n");

        sc.content = tests::listing("hello.cc", "interrupted\n");

        tests::pty p({ prog, "--url", url, "--root", tr.path,
                       "hello.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        p.send("change it\r");

        until(p, [](const tests::vt& v) { return !question_on(v).empty(); },
              30.0);

        const auto began = std::chrono::steady_clock::now();

        p.send("\x03");

        const int status = p.wait_for_exit(15.0);
        const double took = seconds_since(began);

        ok("  it leaves", status == 0, std::to_string(status));

        // **The measurement with teeth.** Left blocked, the worker would sit
        // out the whole two-second grace and be `_Exit`ed past with the
        // "still in flight" line. Abandoned first, it wakes, writes nothing,
        // and is joined -- inside the grace and silently.
        ok("  promptly, rather than sitting out the grace", took < 1.5,
           std::to_string(took) + "s");

        ok("  and joins the worker rather than abandoning it",
           p.seen().find("still in flight") == std::string::npos);

        ok("  with nothing written", tr.get("hello.cc") == "original\n",
           "[" + tr.get("hello.cc") + "]");
    }

    {
        std::cout << "\njcode asks the same question, through the same policy:\n";

        // **jcode's asking path had never been run by a test.** sys::run
        // leaves a child's stdin inherited from whatever runs `make check`,
        // which is why app_jcode_run_test passes --yes on every run -- a
        // [y/N] reading the runner's terminal would hang the suite. A pty is
        // a stdin this file controls.
        tests::tree tr;

        tr.put("hello.cc", "original\n");

        const std::string away = "/tmp/jcode_escape_" +
                                 tr.path.substr(tr.path.find_last_of('/') + 1) +
                                 ".cc";

        ::unlink(away.c_str());

        sc.content = tests::listing(away, "escaped\n") +
                     tests::listing("hello.cc", "accepted\n");

        tests::pty p({ jcode, "--url", url, "--root", tr.path,
                       "change them", "hello.cc" }, ROWS, COLS);

        int prompts = 0;
        int status = -1;

        for(int i = 0; i < 300; i++) {
            p.settle(0.1);

            int seen = 0;

            for(std::size_t at = p.seen().find("[y/N]"); at != std::string::npos;
                at = p.seen().find("[y/N]", at + 1)) seen++;

            if(seen > prompts) { prompts = seen; p.send("n\r"); }

            status = p.wait_for_exit(0.0);

            if(status != -1) break;
        }

        ok("  jcode asks once, as juanaco did", prompts == 1,
           std::to_string(prompts) + " prompts");

        ok("  about the file that could be written",
           p.seen().find("write hello.cc? 1 to 1 lines [y/N]") != std::string::npos);

        ok("  and no leaves it alone", tr.get("hello.cc") == "original\n",
           "[" + tr.get("hello.cc") + "]");

        ok("  saying so in jcode's voice",
           p.seen().find("jcode: left hello.cc alone") != std::string::npos);

        ::unlink(away.c_str());
    }

    {
        std::cout << "\njcode --yes and juanaco's yes write the same bytes:\n";

        // Hunks, so apply() resolves search text against the file and the
        // comparison is of two programs doing real work, not of two copies
        // of what the model sent.
        const std::string original = "// first\n"
                                     "int main() {\n"
                                     "    return 1;\n"
                                     "}\n";

        sc.content = hunks_reply("main.cc");

        tests::tree by_jcode;
        tests::tree by_juanaco;

        by_jcode.put("main.cc", original);
        by_juanaco.put("main.cc", original);

        std::string out, err;

        int js = -1;

        try {
            js = sys::run({ jcode, "--url", url, "--root", by_jcode.path,
                            "--yes", "--format", "hunks", "change it",
                            "main.cc" }, out, err);
        }
        catch(std::exception& e) { err = e.what(); }

        ok("  jcode --yes succeeds", js == 0, std::to_string(js) + " " + err);

        tests::pty p({ prog, "--url", url, "--root", by_juanaco.path,
                       "main.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        p.send("/mode hunks\r");

        until(p, [](const tests::vt& v) { return v.shows("-- edits hunks"); },
              10.0);

        p.send("change it\r");

        const int n = answer_every(p, "y");

        ok("  juanaco asked once and was told yes", n == 1, std::to_string(n));

        const std::string a = by_jcode.get("main.cc");
        const std::string b = by_juanaco.get("main.cc");

        ok("  both changed the file", a != original && b != original,
           "[" + a + "] [" + b + "]");

        ok("  and byte for byte the same", a == b, "[" + a + "] vs [" + b + "]");

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);
    }

    {
        std::cout << "\na reply that arrives a piece at a time:\n";

        tests::tree tr;

        tr.put("hello.cc", "int main() { return 1; }\n");

        // Long enough to take many chunks at seven characters each, so there
        // is a middle of the reply to catch it in.
        sc.content = tests::listing("hello.cc",
                                    "int main() {\n"
                                    "    // one\n"
                                    "    // two\n"
                                    "    // three\n"
                                    "    // four\n"
                                    "    return 0;\n"
                                    "}\n");

        sc.chunk_delay = 0.06;

        tests::pty p({ prog, "--url", url, "--root", tr.path, "--yes",
                       "--task", "watch it arrive", "hello.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        p.send("write it out\r");

        // **Caught in the middle, which is the whole assertion.** The head of
        // the reply has to be on screen while the tail has not arrived; an
        // endpoint that answered at once would satisfy "the reply is shown"
        // for a client that waited for all of it.
        bool mid = false;

        for(int i = 0; i < 400 && !mid; i++) {
            const tests::vt v = shown(p);

            mid = v.shows("Here you go.") && !v.shows("** written hello.cc");

            if(!mid) p.settle(0.02);
        }

        ok("  the head of the reply is on screen before the tail arrives", mid,
           shown(p).dump());

        ok("  and the status line says it is streaming, not waiting",
           mid && shown(p).row(STATUS_ROW).find("streaming") !=
                      std::string::npos,
           "[" + shown(p).row(STATUS_ROW) + "]");

        until(p, [](const tests::vt& v) {
            return v.shows("** written hello.cc");
        }, 30.0);

        {
            const tests::vt v = shown(p);

            // **One entry, not one per chunk.** The transcript is re-wrapped
            // from text on every draw, so fragments that did not accumulate
            // would wrap to one row each -- `    // two` appearing on its own
            // row is what that looks like, and it is what `extend` prevents.
            ok("  the fragments accumulated into one entry",
               v.find("    // two") >= 0 &&
                   v.find("    // three") == v.find("    // two") + 1,
               "rows " + std::to_string(v.find("    // two")) + " and " +
                   std::to_string(v.find("    // three")));

            ok("  and the whole reply still parsed and applied",
               tr.get("hello.cc").find("// four") != std::string::npos,
               "[" + tr.get("hello.cc") + "]");
        }

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);

        sc.chunk_delay = 0;
    }

    {
        std::cout << "\nEscape, mid-reply:\n";

        tests::tree tr;

        tr.put("hello.cc", "original\n");

        sc.content = tests::listing("hello.cc", "this should never land\n");

        // Slow enough that there is a mid-reply to press Escape in.
        sc.chunk_delay = 0.25;

        tests::pty p({ prog, "--url", url, "--root", tr.path, "--yes",
                       "--task", "escape mid-reply", "hello.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        p.send("start writing\r");

        const bool arriving = until(p, [](const tests::vt& v) {
            return v.shows("Here you go.") && !v.shows("** written hello.cc");
        }, 30.0);

        ok("  the reply starts arriving", arriving, shown(p).dump());

        p.send("\x1b");

        until(p, [](const tests::vt& v) { return v.shows("cancelled"); }, 10.0);

        {
            const tests::vt v = shown(p);

            ok("  Escape says the reply was cancelled",
               v.shows("cancelled -- the reply so far is kept"), v.dump());

            // **What arrived is kept.** A cancel that cleared the pane would
            // throw away the only thing the user had, and the point of
            // stopping a reply is usually that its beginning was enough.
            ok("  and what had arrived is still in the transcript",
               v.shows("Here you go."), v.dump());

            // **The note goes last, and a live run against a real model is
            // what put this assertion here.** The sink refuses at the next
            // chunk, so tokens keep arriving for a moment after the key --
            // and a note added on the keypress closed the streaming entry,
            // so the rest of the reply appeared *below* it:
            //
            //     To
            //     -- cancelled -- the reply so far is kept
            //      rewrite `hello.cc` as a long heavily commented ...
            //
            // So the cancel note has to be the last thing in the transcript.
            // Nothing in the scripted sections noticed; the screen of a real
            // 7B reply did.
            int last = -1;

            for(int rr = FIRST_ROW; rr <= LAST_ROW; rr++)
                if(!v.row(rr).empty()) last = rr;

            ok("  and the note is the last line, not spliced into the reply",
               last >= 0 &&
                   v.row(last).find("cancelled") != std::string::npos,
               "last row " + std::to_string(last) + " [" +
                   (last >= 0 ? v.row(last) : std::string()) + "]");

            // A user-initiated stop is not a defect, and the machinery would
            // spell it "the conversation failed -- failed: cancelled".
            ok("  and a cancel is not reported as a failure",
               !v.shows("the conversation failed"), v.dump());
        }

        // **The cancel is not instant and the status line says so.** The sink
        // refuses at the next chunk, the exchange throws, `converse` turns
        // that into an ending and the worker unwinds -- so there is a stretch
        // of `cancelling` before `idle`, and a prompt typed during it is
        // refused like any other. Waiting for idle is what a user does; the
        // first version of this test asserted into the gap and failed.
        const bool settled = until(p, [](const tests::vt& v) {
            return v.row(STATUS_ROW).find("idle") != std::string::npos;
        }, 15.0);

        ok("  and it settles back to idle rather than sticking", settled,
           "[" + shown(p).row(STATUS_ROW) + "]");

        // **Checked after it has settled, and a sabotage is why.** This sat
        // immediately after the cancel note and still passed with the cancel
        // taken out of the sink: the reply had not finished arriving yet, so
        // the file was untouched for a reason that had nothing to do with
        // Escape. The note above is printed by the UI the moment the key is
        // pressed, so it says the key was seen and nothing about whether the
        // work stopped. **This** is the assertion that says that.
        ok("  and nothing was ever written",
           tr.get("hello.cc") == "original\n",
           "[" + tr.get("hello.cc") + "]");

        // **The conversation survives**, which is jalpaca's rule: Escape
        // stops a reply, it does not end the session. A second prompt has to
        // go out and be answered.
        sc.chunk_delay = 0;
        sc.content = tests::listing("hello.cc", "the second reply landed\n");

        p.send("try again\r");

        until(p, [](const tests::vt& v) {
            return v.shows("** written hello.cc");
        }, 30.0);

        ok("  a prompt after a cancel is still answered",
           tr.get("hello.cc") == "the second reply landed\n",
           "[" + tr.get("hello.cc") + "]");

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);
    }

    {
        std::cout << "\nEscape during the silent stretch:\n";

        tests::tree tr;

        tr.put("hello.cc", "original\n");

        sc.content = tests::listing("hello.cc", "too late\n");

        // Head sent, then silence -- which is prefill, and is the one stretch
        // where the sink is never called, so Escape cannot land immediately.
        sc.first_delay = 3.0;

        tests::pty p({ prog, "--url", url, "--root", tr.path, "--yes",
                       "hello.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        p.send("this will be abandoned\r");

        until(p, [](const tests::vt& v) {
            return v.row(STATUS_ROW).find("waiting") != std::string::npos;
        }, 20.0);

        p.send("\x1b");

        until(p, [](const tests::vt& v) {
            return v.shows("takes effect at the first token");
        }, 10.0);

        {
            const tests::vt v = shown(p);

            // **The honest version.** It would be easy to print "cancelled"
            // here and leave the request running, which is the lie this whole
            // arc keeps finding. The gap is #451's to close.
            ok("  it says the cancel is recorded, not done",
               v.shows("takes effect at the first token"), v.dump());

            ok("  and the status line says cancelling",
               v.row(STATUS_ROW).find("cancelling") != std::string::npos,
               "[" + v.row(STATUS_ROW) + "]");
        }

        // And then it is honoured, at the first chunk.
        until(p, [](const tests::vt& v) {
            return v.row(STATUS_ROW).find("idle") != std::string::npos;
        }, 30.0);

        ok("  and it is honoured once something arrives",
           shown(p).row(STATUS_ROW).find("idle") != std::string::npos,
           "[" + shown(p).row(STATUS_ROW) + "]");

        // After idle, for the same reason: before it, an untouched file
        // proves only that the reply had not arrived yet.
        ok("  with nothing written", tr.get("hello.cc") == "original\n",
           "[" + tr.get("hello.cc") + "]");

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);

        sc.first_delay = 0;
    }

    {
        std::cout << "\n--no-stream, which is the only path with a token count:\n";

        tests::tree tr;

        tr.put("hello.cc", "int main() { return 1; }\n");

        sc.content = tests::listing("hello.cc", "int main() { return 0; }\n");

        tests::pty p({ prog, "--url", url, "--root", tr.path, "--yes",
                       "--no-stream", "--task", "no stream", "hello.cc" },
                     ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        p.send("make it return zero\r");

        until(p, [](const tests::vt& v) {
            return v.shows("** written hello.cc");
        }, 30.0);

        ok("  it still works without streaming",
           tr.get("hello.cc") == "int main() { return 0; }\n",
           "[" + tr.get("hello.cc") + "]");

        ok("  and did not ask for a stream",
           sc.last_body.find("\"stream\"") == std::string::npos,
           sc.last_body.substr(0, 200));

        // The usage block only exists here, which is what --no-stream buys.
        ok("  the token count arrives, which streaming cannot give",
           shown(p).row(STATUS_ROW).find(" 0 in / 0 out") == std::string::npos,
           "[" + shown(p).row(STATUS_ROW) + "]");

        ok("  and the drift line with it",
           shown(p).shows("the server counted"), shown(p).dump());

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);
    }

    {
        std::cout << "\n/model, against what the server lists:\n";

        tests::tree tr;

        tr.put("hello.cc", "int main() { return 1; }\n");

        sc.content = tests::listing("hello.cc", "int main() { return 0; }\n");

        sc.models = { oa::listing{ "m", 4096 }, oa::listing{ "other", 2048 } };

        tests::pty p({ prog, "--url", url, "--root", tr.path, "--yes",
                       "hello.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("offered:"); }, 10.0);

        ok("  the list arrives at startup, each with its context",
           shown(p).shows("offered: m (4096), other (2048)"), shown(p).dump());

        // **Two offered and none chosen, so the ceiling is not guessed.**
        // jserve uses the first, but the protocol does not say so, and a
        // ceiling read off the wrong model is the number #437's warning
        // would be judged against.
        ok("  with two offered and none chosen, the ceiling is not guessed",
           shown(p).row(STATUS_ROW).find("ctx 0/?") != std::string::npos,
           "[" + shown(p).row(STATUS_ROW) + "]");

        p.send("/model nosuch\r");

        until(p, [](const tests::vt& v) {
            return v.shows("no model called nosuch");
        }, 10.0);

        ok("  a name the server does not offer is refused, now",
           shown(p).shows("no model called nosuch"), shown(p).dump());

        ok("  and changes nothing",
           shown(p).row(0).find("(server's default)") != std::string::npos,
           shown(p).row(0));

        p.send("/model other\r");

        until(p, [](const tests::vt& v) {
            return v.shows("the next request goes to other");
        }, 10.0);

        {
            const tests::vt v = shown(p);

            ok("  choosing one says what that means",
               v.shows("the next request goes to other"), v.dump());

            ok("  the top line names it", v.row(0).find("other") != std::string::npos,
               v.row(0));

            ok("  and the ceiling is that model's",
               v.row(STATUS_ROW).find("/2048") != std::string::npos,
               "[" + v.row(STATUS_ROW) + "]");
        }

        p.send("make it return zero\r");

        until(p, [](const tests::vt& v) { return v.shows("written hello.cc"); },
              20.0);

        // **The assertion with teeth.** `/model NAME` set a field the header
        // drew from and every request was built from `--model`, so from
        // phase 2 until #450 choosing a model changed the top line and
        // nothing else -- and every assertion above would have passed.
        ok("  and the request really asks for it",
           sc.last_body.find("\"model\": \"other\"") != std::string::npos,
           sc.last_body.substr(0, 160));

        p.send("/model\r");

        until(p, [](const tests::vt& v) { return v.shows("* other"); }, 10.0);

        ok("  /model lists them, marking the one in use",
           shown(p).shows("* other   context 2048") &&
               shown(p).shows("  m   context 4096"), shown(p).dump());

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);

        sc.models = std::vector<oa::listing>(1, oa::listing{ "m", 4096 });
    }

    {
        std::cout << "\n/files, after a layout that left something out:\n";

        tests::tree tr;

        tr.put("hello.cc", "int main() { return 1; }\n");

        // Far over the budget below, so lay_out() has to drop it -- whole,
        // never truncated, and with its reason recorded.
        tr.put("big.cc", std::string(20000, 'x') + "\n");

        sc.content = tests::listing("hello.cc", "int main() { return 0; }\n");

        tests::pty p({ prog, "--url", url, "--root", tr.path, "--yes",
                       "--context", "1500", "hello.cc", "big.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("offered:"); }, 10.0);

        p.send("/files\r");

        until(p, [](const tests::vt& v) {
            return v.shows("nothing laid out until a request goes out");
        }, 10.0);

        ok("  before a request, it says nothing has been laid out yet",
           shown(p).shows("nothing laid out until a request goes out"),
           shown(p).dump());

        p.send("make it return zero\r");

        until(p, [](const tests::vt& v) { return v.shows("written hello.cc"); },
              20.0);

        p.send("/files\r");

        until(p, [](const tests::vt& v) { return v.shows("  sent  hello.cc"); },
              10.0);

        {
            const tests::vt v = shown(p);

            ok("  afterwards, what the model saw", v.shows("  sent  hello.cc"),
               v.dump());

            ok("  and what it did not, with the reason",
               v.shows("  left out  big.cc (") && v.shows("of 1500 already used)"),
               v.dump());

            ok("  against the budget that decided it",
               v.shows("against a budget of 1500"), v.dump());
        }

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);
    }

    {
        std::cout << "\n/stats, across two requests:\n";

        tests::tree tr;

        tr.put("hello.cc", "int main() { return 1; }\n");

        sc.content = tests::listing("hello.cc", "int main() { return 0; }\n");

        tests::pty p({ prog, "--url", url, "--root", tr.path, "--yes",
                       "hello.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("offered:"); }, 10.0);

        p.send("/stats\r");

        until(p, [](const tests::vt& v) {
            return v.shows("nothing has been sent yet");
        }, 10.0);

        ok("  before anything is sent it says so",
           shown(p).shows("nothing has been sent yet"), shown(p).dump());

        p.send("make it return zero\r");

        until(p, [](const tests::vt& v) { return v.shows("written hello.cc"); },
              20.0);

        until(p, [](const tests::vt& v) {
            return v.row(STATUS_ROW).find("idle") != std::string::npos;
        }, 10.0);

        // A second request: the file already says what the model sends, so
        // nothing is written -- and the tokens still count.
        p.send("again\r");

        until(p, [](const tests::vt& v) {
            return v.shows("already what the model sent");
        }, 20.0);

        until(p, [](const tests::vt& v) {
            return v.row(STATUS_ROW).find("idle") != std::string::npos;
        }, 10.0);

        p.send("/stats\r");

        until(p, [](const tests::vt& v) { return v.shows("across 2 request(s)"); },
              10.0);

        {
            const tests::vt v = shown(p);

            // **The assertion with teeth.** The counts event carried a
            // per-request running total and absorb() *assigned* it, so the
            // session's tokens went back to the last request's at every new
            // prompt. 20 a request, twice, is 40 -- not 20.
            ok("  tokens add up across requests, rather than restarting",
               v.shows(" 40 out, across 2 request(s)"), v.dump());

            ok("  against the model's ceiling", v.shows(" of 4096 ("), v.dump());

            ok("  and the last request's consumption, against its bounds",
               v.shows("last request: 1 of 8 rounds, 1 of 3 attempts"), v.dump());

            ok("  and the status line agrees about the ceiling",
               v.row(STATUS_ROW).find("/4096") != std::string::npos,
               "[" + v.row(STATUS_ROW) + "]");
        }

        p.send("/quit\r");

        ok("  /quit leaves", p.wait_for_exit(10.0) == 0);
    }

    {
        std::cout << "\na server that never answers:\n";

        sc.hang = true;

        tests::tree tr;

        tr.put("hello.cc", "original\n");

        tests::pty p({ prog, "--url", url, "--root", tr.path, "--yes",
                       "hello.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        p.send("this will not come back\r");

        // **"waiting", not "prefill".** Nothing is on the wire, and from the
        // client's side a server that is prefilling and one that has died
        // are the same thing -- which is #451 in one assertion.
        const bool waiting = until(p, [](const tests::vt& v) {
            return v.row(STATUS_ROW).find("waiting") != std::string::npos;
        }, 20.0);

        ok("  the status line says it is waiting, with a clock", waiting,
           "[" + shown(p).row(STATUS_ROW) + "]");

        // The clock is the only thing that changes during the silent
        // stretch, so it is the only proof the screen is still being painted.
        p.settle(2.5);

        ok("  and the clock advances while nothing arrives",
           shown(p).row(STATUS_ROW).find("waiting 0s") == std::string::npos,
           "[" + shown(p).row(STATUS_ROW) + "]");

        // **The screen keeps repainting through a blocking request**, which
        // is the whole reason the worker is on a thread. A slash command is
        // answered by the UI thread, so an answer appearing here is proof
        // that thread is not inside the HTTP read.
        p.send("/mode hunks\r");

        until(p, [](const tests::vt& v) { return v.shows("-- edits hunks"); },
              10.0);

        ok("  and the UI answers a command while it waits",
           shown(p).shows("-- edits hunks"), shown(p).dump());

        p.send("and so is this\r");

        until(p, [](const tests::vt& v) { return v.shows("still working"); },
              10.0);

        {
            const tests::vt v = shown(p);

            ok("  a second request is refused", v.shows("still working"),
               v.dump());

            ok("  and the line stays in the prompt rather than being eaten",
               v.row(PROMPT_ROW).find("> and so is this") != std::string::npos,
               "[" + v.row(PROMPT_ROW) + "]");

            ok("  so it did not reach the transcript",
               !in_transcript(v, "> and so is this"),
               "found on row " + std::to_string(v.find("> and so is this")));
        }

        // **Ctrl-U, and it is not a flourish.** The refused line is still
        // in the prompt, so anything typed next is appended to it -- the
        // first version of this test sent "/quit" into "and so is this" and
        // got "and so is this/quit", which is not a command, so it was
        // refused in turn and the program could only be left with Ctrl-C.
        // That is how the clear came to exist; see the key handler.
        p.send("\x15");

        until(p, [](const tests::vt& v) {
            return v.row(PROMPT_ROW).find("and so is this") ==
                   std::string::npos;
        }, 10.0);

        ok("  Ctrl-U clears it, so the refusal is not a trap",
           shown(p).row(PROMPT_ROW).find("and so is this") ==
               std::string::npos,
           "[" + shown(p).row(PROMPT_ROW) + "]");

        // **The measurement this whole section exists for.** The request
        // timeout is ten minutes and `relay`'s destructor joins, so a
        // juanaco that waited for its worker would sit here for the whole of
        // it with the terminal already handed back.
        const auto began = std::chrono::steady_clock::now();

        p.send("/quit\r");

        // **Polled rather than read after the exit**, which is the second
        // thing this section taught. `wait_for_exit` returns the moment
        // `waitpid` reaps the child, and the message is written immediately
        // before `_Exit` -- so the bytes are still in the pty when the
        // assertion runs and reading `seen()` afterwards found nothing. It is
        // a stream assertion on purpose: the message goes to stderr after
        // endwin, so there is no screen left to reconstruct it from.
        bool said = false;

        for(int i = 0; i < 300 && !said; i++) {
            said = p.seen().find("still in flight") != std::string::npos;

            if(!said) p.settle(0.1);
        }

        const int status = p.wait_for_exit(30.0);
        const double took = seconds_since(began);

        ok("  /quit leaves anyway", status == 0, std::to_string(status));

        ok("  within the couple of seconds it allows itself, not the "
           "ten-minute timeout",
           took < 10.0, std::to_string(took) + "s");


        ok("  and says that it did not wait", said);

        ok("  nothing was written", tr.get("hello.cc") == "original\n",
           "[" + tr.get("hello.cc") + "]");

        sc.hang = false;
    }

    sc.closing = true;

    server.stop();
    th.join();

    // What a green run does not establish.
    //
    // **Nothing about a model.** Every reply here was written in this file.
    // What a model actually sends is untested by anything in `make check` and
    // needs a GGUF, which is #236's bargain one app over.
    //
    // **Not streaming**, which does not exist yet (#448) -- and one assertion
    // above is written to fail when it does.
    //
    // **Not the confirm dialog** (#449). The two paths tested are --yes and
    // nothing-is-written; the one in between, where a user is asked, cannot
    // be written until the keystroke can travel to the worker.
    //
    // **Not a tool call and not a build.** The scripted endpoint asks for
    // neither, so `until_built`'s second attempt and the `build` rows in the
    // transcript have never been drawn by a test.
    //
    // **Not cancellation.** `/quit` during a request is checked; Escape is
    // #448, and the exchange's `wanted()` check is reached only at a round
    // boundary, which no reply here has more than one of.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": "
              << failures << " failure(s)\n";

    return failures ? 1 : 0;
}
