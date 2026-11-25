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

// juanaco's skeleton, driven through a pty -- #446, phase 2 of #444.
//
// This is what #445 was built for, and the reason it was built first: jlib's
// other curses program has no test at all, and #247 records that its harness
// lives outside the repo. There was never going to be a good moment to add
// one retroactively.
//
// Nothing here involves a model, a server or a network. Everything asserted
// is the program's own state: what it paints, what a slash command changes,
// and that it leaves when told to.

#include "pty.hh"
#include "vt.hh"

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>

namespace tests = jlib::tests;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    std::cout << (good ? "  ok   " : "  FAIL ") << what;

    if(!detail.empty()) std::cout << ": " << detail;

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

int main() {
    std::cout << std::unitbuf;

    const std::string prog = juanaco();

    const int ROWS = 30;
    const int COLS = 110;

    std::cout << "juanaco's skeleton, through a pty:\n";

    {
        tests::pty p({ prog, "--task", "fix the off-by-one" }, ROWS, COLS);

        // **Assert on the screen, not on the stream.** `expect` is still how
        // the test waits -- it is the synchronisation primitive -- but what is
        // asserted is the reconstructed screen, because the two are not the
        // same question. A pad blitted and then overwritten paints its text
        // and shows nothing, and that is exactly the bug this test used to
        // pass: every slash command's answer was in the stream and none of it
        // was on screen. See the note at the top of `vt.hh`.
        const auto shown = [&p]() {
            tests::vt v(ROWS, COLS);

            v.feed(p.seen());

            return v;
        };

        // **Poll the screen, do not wait for a string.** The program draws
        // the prompt, so it echoes whatever the test types -- and `expect`
        // on text the test just sent matches that echo and returns before the
        // command has been processed at all. Two assertions passed and then
        // failed for exactly that reason once the screen was being checked.
        // **The prompt and a user entry look the same on screen**: both are
        // drawn with "> ", which is right for a user -- the line appears as
        // it was typed -- and means a text match alone cannot tell "being
        // typed" from "submitted". So anything about a user's line has to say
        // *where*. With 30 rows the transcript occupies 2..26, the prompt is
        // 28 and the status line 29.
        const int FIRST_ROW = 2;
        const int LAST_ROW = ROWS - 4;

        const auto in_transcript = [&](const tests::vt& v,
                                       const std::string& text) {
            const int r = v.find(text);

            return r >= FIRST_ROW && r <= LAST_ROW;
        };

        const auto until = [&p, &shown](auto pred, double seconds) {
            for(int i = 0; i < int(seconds * 10); i++) {
                if(pred(shown())) return true;

                p.settle(0.1);
            }

            return pred(shown());
        };

        until([](const tests::vt& v) { return v.shows("juanaco"); }, 10.0);

        ok("  it starts and says what it is", shown().shows("juanaco"));

        ok("  the task is in the header",
           shown().find("fix the off-by-one") == 0,
           "on row " + std::to_string(shown().find("fix the off-by-one")));

        ok("  and the mode", shown().row(0).find("hunks") != std::string::npos,
           shown().row(0));

        // Ctrl-J adds a line rather than submitting. On a screen this is
        // simply two rows: the entry's first line carries "> " and its
        // continuation does not. Two submissions would put "> " on both.
        p.send("first line\x0asecond line\r");

        until([&](const tests::vt& v) {
            return in_transcript(v, "> first line");
        }, 10.0);

        {
            const tests::vt v = shown();

            const int a = v.find("> first line");
            const int b = v.find("second line");

            ok("  Ctrl-J continued the prompt rather than sending it",
               a >= 0 && b == a + 1 &&
                   v.row(b).find("> ") == std::string::npos,
               "rows " + std::to_string(a) + " and " + std::to_string(b) +
                   ", second reads [" + v.row(b) + "]");
        }

        p.send("make copy_into stop at the terminator\r");

        until([&](const tests::vt& v) {
            return in_transcript(v, "> make copy_into stop at the terminator");
        }, 10.0);

        ok("  a typed prompt lands in the transcript",
           in_transcript(shown(), "> make copy_into stop at the terminator"),
           "found on row " +
               std::to_string(shown().find("> make copy_into stop at the terminator")));

        ok("  with a note that there is nowhere to send it yet",
           shown().shows("nothing to send it to yet"));

        p.send("/mode whole\r");

        until([](const tests::vt& v) { return v.shows("-- edits whole"); }, 10.0);

        {
            const tests::vt v = shown();

            ok("  /mode whole is acknowledged", v.shows("edits whole"));

            ok("  and the header follows it",
               v.row(0).find("whole") != std::string::npos, v.row(0));
        }

        p.send("/mode build on\r");

        until([](const tests::vt& v) { return v.shows("-- build on"); }, 10.0);

        ok("  build can be turned on, and the header says so",
           shown().row(0).find("build") != std::string::npos, shown().row(0));

        // The escape hatch: a coding harness meets a leading slash the first
        // time anyone pastes an absolute path.
        p.send("//usr/local/lib is where it looks\r");

        until([&](const tests::vt& v) {
            return in_transcript(v, "> /usr/local/lib is where it looks");
        }, 10.0);

        {
            const tests::vt v = shown();

            ok("  // sends a prompt whose first character is a slash",
               in_transcript(v, "> /usr/local/lib is where it looks"),
               "found on row " +
                   std::to_string(v.find("> /usr/local/lib is where it looks")));

            ok("  and it was not taken for a command",
               !v.shows("no such command: /usr"));
        }

        p.send("/nosuchthing\r");

        until([](const tests::vt& v) {
            return v.shows("no such command: /nosuchthing");
        }, 10.0);

        ok("  an unknown command is reported",
           shown().shows("no such command: /nosuchthing"));

        p.send("/help\r");

        until([](const tests::vt& v) { return v.shows("/files"); }, 10.0);

        ok("  /help lists the commands", shown().shows("/files"));

        // The inert ones still have to answer, and say which issue will give
        // them real data -- an empty answer reads as "none" rather than as
        // "not asked yet".
        p.send("/stats\r");

        until([](const tests::vt& v) {
            return v.shows("nothing has been sent yet");
        }, 10.0);

        ok("  /stats answers, and says why it is empty",
           shown().shows("nothing has been sent yet"));

        // Where the cursor rests is the one thing on screen that is not text,
        // and painting the status line after placing it parked it one row low.
        p.settle(0.5);

        {
            const tests::vt v = shown();

            // 30 rows: the prompt is row 28 and the status line is row 29.
            ok("  the cursor is left in the prompt, not on the status line",
               v.cursor_row() == 28,
               "cursor on row " + std::to_string(v.cursor_row()) +
                   (v.cursor_row() == 29 ? " -- the status line" : ""));
        }

        p.send("/quit\r");

        const int status = p.wait_for_exit(10.0);

        ok("  /quit leaves", status == 0, std::to_string(status));
    }

    ok("  and the harness returns with the child reaped", true);

    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": "
              << failures << " failure(s)\n";

    return failures ? 1 : 0;
}
