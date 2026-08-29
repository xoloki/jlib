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

// The projection dump, which is about to be an oracle.
//
// #20, #24 and #28 all change the projection chain, and the apps that show it
// are looked at rather than asserted.  The plan is to capture a reference dump
// before each change and diff it after, which only works if the dump itself is
// trustworthy -- so it gets a test before it gets that job.

#include <jlib/math/dump.hh>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace dump = jlib::math::dump;

using jlib::math::vertex;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static const char* PATH = "math_dump_test.out";

/** Run `frames` frames of `n` vertices each, under the given configuration. */
static std::vector<std::string> run(const char* skip, const char* want,
                                    int frames, int n)
{
    ::setenv("JLIB_PLOT_DUMP", PATH, 1);

    if(skip) ::setenv("JLIB_PLOT_DUMP_SKIP", skip, 1);
    else     ::unsetenv("JLIB_PLOT_DUMP_SKIP");

    if(want) ::setenv("JLIB_PLOT_DUMP_FRAMES", want, 1);
    else     ::unsetenv("JLIB_PLOT_DUMP_FRAMES");

    dump::reset();

    for(int f = 0; f < frames; f++) {
        for(int i = 0; i < n; i++) {
            vertex<double> src(3), dst(3);

            // Distinguishable per frame and per vertex, so the file says which
            // is which rather than merely how many.
            src[0] = f;
            src[1] = i;
            dst[0] = f * 100 + i;

            dump::vertex("t", src, dst);
        }

        dump::frame_done();
    }

    // Closed by frame_done() when it has what it wants; reset() closes it when
    // the run ended early.
    dump::reset();

    std::vector<std::string> lines;
    std::ifstream in(PATH);
    std::string line;

    while(std::getline(in, line))
        lines.push_back(line);

    return lines;
}

static int markers(const std::vector<std::string>& lines) {
    int n = 0;

    for(const std::string& l : lines)
        if(l.compare(0, 6, "frame ") == 0) n++;

    return n;
}

static void one_frame_by_default() {
    std::cout << "\none frame by default:\n";

    const std::vector<std::string> l = run(0, 0, 5, 2);

    ok("five frames offered, one captured", markers(l) == 1,
       std::to_string(markers(l)) + " frames");

    ok("and it is frame zero", !l.empty() && l.front() == "frame 0",
       l.empty() ? "empty" : l.front());

    // Two vertices plus the marker.  The default is what every existing caller
    // gets, so it is the line that must not move.
    ok("with both its vertices", l.size() == 3, std::to_string(l.size()) + " lines");
}

static void asking_for_more_gets_more() {
    std::cout << "\nasking for more gets more:\n";

    const std::vector<std::string> l = run(0, "3", 5, 2);

    ok("three frames captured", markers(l) == 3, std::to_string(markers(l)));

    ok("numbered from zero, in order",
       l.size() > 6 && l[0] == "frame 0" && l[3] == "frame 1" && l[6] == "frame 2",
       l.size() > 6 ? l[0] + " / " + l[3] + " / " + l[6] : "too short");

    // The point of capturing several: it stops after what was asked for, so a
    // long run does not fill a disk.
    ok("and it stops after the third", l.size() == 9,
       std::to_string(l.size()) + " lines");
}

static void skipping_reaches_a_later_frame() {
    std::cout << "\nskipping reaches a later frame:\n";

    const std::vector<std::string> l = run("3", "2", 8, 1);

    ok("two frames captured", markers(l) == 2, std::to_string(markers(l)));

    // The numbering counts frames of the whole run, not of the file, which is
    // what makes a skipped dump comparable with an unskipped one.
    ok("numbered by the run, not by the file",
       l.size() > 2 && l[0] == "frame 3" && l[2] == "frame 4",
       l.size() > 2 ? l[0] + " / " + l[2] : "too short");

    ok("and the skipped frames left nothing behind", l.size() == 4,
       std::to_string(l.size()) + " lines");
}

static void a_frame_that_drew_nothing_costs_nothing() {
    std::cout << "\na frame that drew nothing costs nothing:\n";

    ::setenv("JLIB_PLOT_DUMP", PATH, 1);
    ::unsetenv("JLIB_PLOT_DUMP_SKIP");
    ::setenv("JLIB_PLOT_DUMP_FRAMES", "1", 1);

    dump::reset();

    // Two empty frames, then a real one.  If an empty frame consumed the
    // budget the real one would never be seen -- which is the case a plot
    // hits whenever an object list is empty for a frame or two at startup.
    dump::frame_done();
    dump::frame_done();

    vertex<double> src(3), dst(3);
    src[0] = 7;
    dst[0] = 9;

    dump::vertex("t", src, dst);
    dump::frame_done();
    dump::reset();

    std::vector<std::string> lines;
    std::ifstream in(PATH);
    std::string line;

    while(std::getline(in, line)) lines.push_back(line);

    ok("the empty frames wrote nothing", markers(lines) == 1,
       std::to_string(markers(lines)));

    ok("and the budget went to the frame that drew",
       lines.size() == 2 && lines[0] == "frame 2",
       lines.empty() ? "empty" : lines[0]);
}

static void unset_is_off() {
    std::cout << "\nunset is off:\n";

    ::unsetenv("JLIB_PLOT_DUMP");

    dump::reset();

    ok("nothing is active", !dump::active());

    vertex<double> src(3), dst(3);

    dump::vertex("t", src, dst);   // must not crash or create a file
    dump::frame_done();

    ok("and calling it anyway is harmless", true);

    dump::reset();
}

int main() {
    std::cout << std::unitbuf;

    one_frame_by_default();
    asking_for_more_gets_more();
    skipping_reaches_a_later_frame();
    a_frame_that_drew_nothing_costs_nothing();
    unset_is_off();

    std::remove(PATH);

    // What a green run does not establish.
    //
    // That a dump of two different pipelines is comparable.  This checks the
    // framing -- how many frames, which ones, numbered how -- and never calls
    // Plot at all, so it says nothing about whether the coordinates in a real
    // dump mean the same thing on both sides of a change.  That is the job it
    // is about to be given and the reason it needed a test first, not a
    // guarantee this file provides.
    //
    // Nor does skipping make two runs comparable by itself: frame 300 of two
    // runs matches only if both are deterministic to frame 300, which is a
    // property of the app and not of the dump.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
