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

// juanaco, driven through a pty -- #446 and #447, phases 2 and 3 of #444.
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
// the harness to write at all. It is how the status line's "working", the
// refusal of a second request, and leaving without waiting out a ten-minute
// timeout are checked -- none of which can be observed against an endpoint
// that replies in a millisecond.

#include "pty.hh"
#include "scripted.hh"
#include "vt.hh"

#include <jlib/sys/server.hh>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

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

int main() {
    std::cout << std::unitbuf;

    const std::string prog = juanaco();

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

            // **The phase's own bargain, asserted rather than assumed.**
            // `request::str` adds the key only when streaming, so its
            // absence is the proof. This assertion is meant to fail when
            // #448 lands, which is the point of it.
            ok("  and asked for the whole body rather than a stream",
               sc.last_body.find("\"stream\"") == std::string::npos);

            ok("  the layout was reported", v.shows("1 of 1 file(s)"), v.dump());

            // The estimate against what the server counted, which is the only
            // place the true bytes-per-token ratio is visible from this side.
            ok("  and what the token guess was worth",
               v.shows("estimated") && v.shows("the server counted"),
               v.dump());
        }

        // The counters stop being a decoration once something has been sent.
        p.send("/stats\r");

        until(p, [](const tests::vt& v) {
            return v.shows("tokens in,") &&
                   !v.shows("nothing has been sent yet");
        }, 10.0);

        {
            const tests::vt v = shown(p);

            ok("  /stats is no longer all zero",
               !v.shows("nothing has been sent yet"), v.dump());

            ok("  and the status line counted the same request",
               v.row(STATUS_ROW).find(" 0 in / 0 out") == std::string::npos,
               "[" + v.row(STATUS_ROW) + "]");

            ok("  which is back to idle now the worker has finished",
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
        std::cout << "\nwithout --yes, which is #449's bargain:\n";

        tests::tree tr;

        tr.put("hello.cc", "original\n");

        sc.content = tests::listing("hello.cc", "changed\n");

        tests::pty p({ prog, "--url", url, "--root", tr.path,
                       "hello.cc" }, ROWS, COLS);

        until(p, [](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        ok("  it says up front that nothing will be written",
           shown(p).shows("announced and not written"), shown(p).dump());

        p.send("/mode whole\r");

        until(p, [](const tests::vt& v) { return v.shows("-- edits whole"); },
              10.0);

        p.send("change it\r");

        until(p, [](const tests::vt& v) {
            return v.shows("would write hello.cc");
        }, 20.0);

        ok("  an edit is announced", shown(p).shows("would write hello.cc"),
           shown(p).dump());

        // The assertion that matters: not that it announced, but that the
        // file is still there. An announcement and a write look identical
        // from inside the transcript.
        ok("  and the file is untouched", tr.get("hello.cc") == "original\n",
           "[" + tr.get("hello.cc") + "]");

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

        const bool working = until(p, [](const tests::vt& v) {
            return v.row(STATUS_ROW).find("working") != std::string::npos;
        }, 20.0);

        ok("  the status line says a request is in flight", working,
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
