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

// A curses program driven by make check -- #445, phase 1 of #444.
//
// The harness exists before the program it was built for, on purpose. jlib's
// one curses program has never been covered by anything the suite runs: #247
// records that jalpaca's harness lives outside the repo, where #197 had
// already established that harnesses should not. Writing juanaco first would
// have made that two programs instead of one.
//
// ## This binary is both halves
//
// Run with `--child` it *is* the curses program: it paints a line, waits for
// a key, paints what it got, and exits. Run with no arguments it is the test,
// and it re-execs itself with `--child` through a pty.
//
// One binary rather than two because `check_PROGRAMS = $(TESTS)` in this
// directory -- every check program is a test -- and because a separate
// fixture binary is a thing that can drift out of step with the test that
// drives it. There is nothing to keep in sync when it is the same file.

#include "pty.hh"

#include <ncurses.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace tests = jlib::tests;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    std::cout << (good ? "  ok   " : "  FAIL ") << what;

    if(!detail.empty()) std::cout << ": " << detail;

    std::cout << "\n";

    if(!good) failures++;
}

/**
 * The curses half: the smallest program that cannot be driven down a pipe.
 *
 * It calls initscr(), which is the line that fails without a terminal, then
 * paints, then blocks on a keystroke -- the three things a pipe cannot
 * provide and a pty can.
 */
static int child() {
    ::initscr();
    ::cbreak();
    ::noecho();
    ::keypad(::stdscr, TRUE);

    ::mvprintw(0, 0, "PAINTED-FIRST-LINE");
    ::mvprintw(2, 0, "rows=%d cols=%d", LINES, COLS);
    ::refresh();

    const int c = ::getch();

    // Escape is reported as 27, and saying so in the output is what lets the
    // test assert that a control character arrived as itself rather than as
    // whatever a canonical-mode line discipline would have made of it.
    ::mvprintw(4, 0, "GOT-KEY=%d", c);
    ::refresh();

    // A second key, so the test can show that the program is still running
    // and reading after the first -- which is what a transcript pane needs.
    const int d = ::getch();

    ::mvprintw(6, 0, "GOT-SECOND=%d", d);
    ::refresh();

    ::endwin();

    return 0;
}

/** This program's own path, for re-exec. */
static std::string self(const char* argv0) {
    const std::string a = argv0 ? argv0 : "";

    // automake runs a test from the build directory, so argv[0] is usable --
    // but it may be a bare name, which execv will not search for.
    if(a.find('/') == std::string::npos) return "./" + a;

    return a;
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;

    if(argc > 1 && std::string(argv[1]) == "--child") return child();

    const std::string me = self(argv[0]);

    std::cout << "a curses program driven through a pty:\n";

    {
        tests::pty p({ me, "--child" }, 30, 100);

        ok("  it paints", p.expect("PAINTED-FIRST-LINE", 10.0));

        // The size the harness set, read back from what curses saw. This is
        // the assertion that distinguishes a real terminal from a pipe: LINES
        // and COLS come from an ioctl that has nothing to answer it otherwise.
        // **30 by 100, and not 24 by 80.** The default is what curses
        // reports when nothing can answer the ioctl -- terminfo's entry for
        // xterm -- so an assertion on 24 by 80 passes whether or not the
        // harness sets a size at all. The first version of this test did
        // exactly that and passed for the wrong reason.
        //
        // The result first, then the detail from it. Built the other way
        // round the detail is evaluated on success too, and said "painted a
        // size, but not that one" next to an `ok`.
        const bool sized = p.expect("rows=30 cols=100", 10.0);

        ok("  and it sees the window size the harness set", sized,
           sized ? ""
                 : (p.seen().find("rows=") != std::string::npos
                        ? "painted a size, but not that one"
                        : "painted no size at all"));

        // Escape, which is the whole reason jalpaca took the terminal out of
        // canonical mode: in canonical mode this would sit in a line buffer
        // until Return.
        p.send("\x1b");

        ok("  a keystroke arrives without waiting for Return",
           p.expect("GOT-KEY=27", 10.0));

        p.send("A");

        ok("  and it is still reading afterwards", p.expect("GOT-SECOND=65", 10.0));

        const int status = p.wait_for_exit(10.0);

        ok("  it exits on its own", status != -1, std::to_string(status));
    }

    // **What a pipe gives instead**, which is the justification for the whole
    // file. `sys::pstream` would drive the same child this way, so this is
    // not a straw man: it is what the harness exists to replace.
    {
        std::string got;

        // TERM explicitly, so the only difference from the pty run above is
        // the pipe. The container inherits TERM=dumb, under which curses
        // pads with spaces rather than addressing the cursor -- it still
        // paints, but leaving the variable uncontrolled would have made this
        // a comparison of two things at once.
        const std::string cmd = "TERM=xterm " + me + " --child < /dev/null 2>&1";

        if(FILE* f = ::popen(cmd.c_str(), "r")) {
            char buf[4096];
            std::size_t n;

            while((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
                got.append(buf, n);

            ::pclose(f);
        }

        // The detail carries what was captured, because all three of these
        // failing together says "the command did not run" rather than
        // anything about pipes -- and without the detail that is invisible.
        const std::string what = "ran [" + cmd + "], captured "
            + std::to_string(got.size()) + " bytes";

        ok("  down a pipe the same program still starts and paints",
           got.find("PAINTED-FIRST-LINE") != std::string::npos, what);

        ok("  but reports terminfo's default size rather than a real one",
           got.find("rows=24 cols=80") != std::string::npos, what);

        ok("  and cannot be typed at: getch gives ERR",
           got.find("GOT-KEY=-1") != std::string::npos, what);
    }

    // The destructor has run: the master is closed and the child reaped. The
    // assertion that it did not *hang* is that this line is reached at all,
    // which is weaker than it looks only because a hang would show up as the
    // suite's own timeout rather than as a failure here.
    ok("  and the harness returns after the child is gone", true);

    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": "
              << failures << " failure(s)\n";

    return failures ? 1 : 0;
}
