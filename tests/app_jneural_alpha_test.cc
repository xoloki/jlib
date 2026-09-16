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

// A CSV that is not as wide as the network.
//
// This drives the program rather than a library, the way app_jchat_test does,
// because the check it covers is in the app: jneural-alpha's MNIST loader
// takes the input width from the file and its training loop took it from -r
// and -c, whose defaults belong to the image mode.  Nothing compared them, so
// an MNIST row was read 10016 doubles past its end, once per sample (#243).
//
// The reason it is worth a test at all is that the failure was loud once and
// stopped being: the pre-batch code multiplied the sample straight in and
// libjmath refused the shape, and the batch path (#132) copies element by
// element, where there is no shape to disagree with.  A guard against a
// silent failure needs something that would notice it going silent again.

#include <jlib/sys/sys.hh>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace sys = jlib::sys;

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

/**
 * The app itself, beside this test in the build tree.
 *
 * The same three candidates app_jchat_test uses, and not $PATH for the same
 * reason: an installed jneural-alpha from an older build would be tested
 * instead of the one just compiled, and it would pass.
 */
static std::string find_jneural_alpha() {
    const char* names[] = {
        "../jlib/apps/jneural-alpha",
        "./jlib/apps/jneural-alpha",
        "../../build/jlib/apps/jneural-alpha"
    };

    for(const char* n : names)
        if(exists(n))
            return n;

    return "";
}

/** A captured stream, flattened, so a failing assertion stays one line. */
static std::string oneline(std::string s) {
    for(char& c : s)
        if(c == '\n' || c == '\r')
            c = ' ';

    while(!s.empty() && s.back() == ' ')
        s.pop_back();

    return s;
}

/** Two MNIST-shaped rows: a label, then 784 pixels. */
static void write_csv(const std::string& path) {
    std::ofstream ofs(path);

    for(int row = 0; row < 2; row++) {
        ofs << row;

        for(int i = 0; i < 784; i++)
            ofs << "," << ((i + row) % 256);

        ofs << "\n";
    }
}

int main() {
    std::cout << std::unitbuf;

    const std::string alpha = find_jneural_alpha();

    if(alpha.empty()) {
        std::cerr << "no jneural-alpha built here, so nothing to drive\n";

        return 77;
    }

    std::cout << "app_jneural_alpha_test: " << alpha << "\n";

    const std::string csv = "jneural_alpha_width_test.csv";

    write_csv(csv);

    std::string out, err;

    const std::vector<std::string> common{
        alpha, "--train-mnist-path", csv,
        "--train-epochs", "1", "--hidden-nodes", "8", "--output-nodes", "10"
    };

    std::cout << "\na file narrower than the network:\n";

    try {
        std::vector<std::string> argv = common;

        const int rc = sys::run(argv, out, err);

        ok("the run is refused", rc != 0, "exit " + std::to_string(rc));

        ok("  naming the width the file has", err.find("784") != std::string::npos, oneline(err));

        ok("  and the width the flags asked for",
           err.find("10800") != std::string::npos, oneline(err));

        // The load-bearing one.  Refusing *eventually* is what the old shape
        // check did, after parsing 110 MB; this says the guard fires on the
        // first row, before any sample is copied anywhere.
        ok("  before a single epoch is trained",
           out.find("Training epoch") == std::string::npos, oneline(out));

        argv = common;
        argv.push_back("-r");
        argv.push_back("28");
        argv.push_back("-c");
        argv.push_back("28");

        const int good = sys::run(argv, out, err);

        ok("and 28 x 28 is accepted", good == 0,
           "exit " + std::to_string(good) + " " + oneline(err));

        ok("  reaching the training loop",
           out.find("Training epoch") != std::string::npos, oneline(out));
    }
    catch(std::exception& e) {
        std::cerr << "app_jneural_alpha_test: " << e.what() << "\n";
        std::remove(csv.c_str());

        return 1;
    }

    std::remove(csv.c_str());

    // What a green run does not establish.
    //
    // That the arithmetic is right.  Two rows train one epoch of an 8-unit
    // network; nothing here looks at a weight.  This is about the guard on the
    // way in, and the accuracy question belongs to a dataset (#131).
    //
    // That the *image* path agrees with its own dimensions.  It scales what it
    // reads to R x C, so it cannot disagree the same way, and nothing here
    // checks it.
    //
    // Nor that math::matrix would catch the overrun.  It would not -- its
    // operator() is `rep[c * M + r]` with no bounds check, which is why the
    // caller has to do the comparing and why breaking this guard is silent
    // rather than a crash.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
