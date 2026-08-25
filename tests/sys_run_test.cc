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

// Running a program without a shell, and identifying a file without asking a
// shell to do it.
//
// jlib had five places that built a command line by concatenating a string it
// did not choose, and handed the result to /bin/sh.  Two of them ran "rm" and
// "mv" on a mail folder's name.

#include <jlib/sys/sys.hh>
#include <jlib/util/MimeType.hh>
#include <jlib/util/content_type.hh>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static std::string trimmed(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n");

    if(a == s.npos) return std::string();

    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

static void arguments_are_arguments() {
    std::cout << "sys::run:\n";

    std::string out, err;

    ok("it runs a program", jlib::sys::run({ "echo", "hello" }, out, err) == 0 &&
       trimmed(out) == "hello", trimmed(out));

    // Every one of these is punctuation to a shell and a character to
    // everything else.  If any of them were interpreted, the output would
    // differ from the input.
    const std::vector<std::string> nasty = {
        "echo", "a b", "c;d", "$HOME", "`id`", "$(id)", "x&&y", "*", "'q'", "\"r\"",
    };

    jlib::sys::run(nasty, out, err);

    std::string want;

    for(std::size_t i = 1; i < nasty.size(); i++) {
        if(i > 1) want += " ";
        want += nasty[i];
    }

    ok("and passes its arguments through untouched", trimmed(out) == want,
       trimmed(out));

    ok("the exit status comes back",
       jlib::sys::run({ "false" }, out, err) != 0 &&
       jlib::sys::run({ "true" }, out, err) == 0);

    // A program that ran and failed is a status, not an exception.  One that
    // could not be started at all is an exception, because there is no status
    // to report and returning 127 would be indistinguishable from a program
    // that exited 127 on purpose.
    bool threw = false;

    try { jlib::sys::run({ "jlib-no-such-program-xyzzy" }, out, err); }
    catch(std::exception&) { threw = true; }

    ok("a program that is not there throws", threw);

    threw = false;
    try { jlib::sys::run({}, out, err); }
    catch(std::exception&) { threw = true; }

    ok("and so does nothing to run", threw);

    ok("stderr comes back separately",
       jlib::sys::run({ "sh", "-c", "echo o; echo e >&2" }, out, err) == 0 &&
       trimmed(out) == "o" && trimmed(err) == "e",
       trimmed(out) + " / " + trimmed(err));

    // Bigger than a pipe buffer, which is what the temp files are for: a
    // child filling stderr while the parent reads stdout deadlocks a naive
    // two-pipe implementation.
    jlib::sys::run({ "sh", "-c",
                     "i=0; while [ $i -lt 4000 ]; do echo aaaaaaaaaaaaaaaaaaaa;"
                     " echo bbbbbbbbbbbbbbbbbbbb >&2; i=$((i+1)); done" },
                   out, err);

    ok("and neither side deadlocks on a lot of output",
       out.size() > 80000 && err.size() > 80000,
       std::to_string(out.size()) + " / " + std::to_string(err.size()));
}

static void a_filename_that_is_a_command() {
    std::cout << "\na filename chosen by somebody else:\n";

    const fs::path dir = fs::temp_directory_path() / "jlib-run-test";

    fs::remove_all(dir);
    fs::create_directories(dir);

    // The marker goes in the working directory, because that is where a
    // shell invoked by system() would create it -- and so the name itself can
    // stay free of slashes and be a filename rather than a path.
    const std::string tag = "jlib-run-test-pwned";
    const fs::path marker = fs::current_path() / tag;

    fs::remove(marker);

    // A name that is a shell command.  Legal on every filesystem jlib builds
    // for, and exactly what "file "+path handed to /bin/sh would have run.
    const fs::path nasty = dir / ("hello.txt; touch " + tag);

    {
        std::ofstream f(nasty);
        f << "just some text\n";
    }

    ok("the file exists", fs::exists(nasty));

    const std::string type = jlib::util::MimeType::get_type_from_file(nasty.string());

    ok("the marker was not created", !fs::exists(marker), type);

    fs::remove(marker);
    ok("and a type came back",
       jlib::util::content_type::valid(type), type);

    fs::remove_all(dir);
}

static void identifying_a_file() {
    std::cout << "\nMimeType:\n";

    const fs::path dir = fs::temp_directory_path() / "jlib-mime-test";

    fs::remove_all(dir);
    fs::create_directories(dir);

    // The name used to decide the type: every icontains() ran over the whole
    // of file(1)'s output, and that output begins with the filename.
    const fs::path misleading = dir / "JPEG-notes.txt";

    {
        std::ofstream f(misleading);
        f << "This file talks about JPEG and GIF and PNG images.\n";
    }

    const std::string type =
        jlib::util::MimeType::get_type_from_file(misleading.string());

    ok("a text file called JPEG-notes.txt is not an image",
       type.rfind("image/", 0) != 0, type);

    ok("it is text", type.rfind("text/", 0) == 0, type);

    // A real one, by its content rather than its name.
    const fs::path png = dir / "not-called-an-image";

    {
        std::ofstream f(png, std::ios::binary);
        const unsigned char sig[] = {
            0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
            0, 0, 0, 13, 'I', 'H', 'D', 'R',
            0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0,
        };
        f.write(reinterpret_cast<const char*>(sig), sizeof sig);
    }

    ok("and a PNG is one whatever it is called",
       jlib::util::MimeType::get_type_from_file(png.string()) == "image/png",
       jlib::util::MimeType::get_type_from_file(png.string()));

    ok("a file that is not there is octet-stream, not an exception",
       jlib::util::MimeType::get_type_from_file((dir / "absent").string())
           == "application/octet-stream");

    ok("and a blob is identified by its content",
       jlib::util::MimeType::get_type_from_data("%PDF-1.4\n%\xc7\xec\x8f\xa2\n")
           == "application/pdf",
       jlib::util::MimeType::get_type_from_data("%PDF-1.4\n%\xc7\xec\x8f\xa2\n"));

    fs::remove_all(dir);
}

int main() {
    std::cout << std::unitbuf;

    // file(1) is not guaranteed to be installed, and neither is a shell that
    // takes -c.  Exit 77 is SKIP, which is how the tests here report a machine
    // that cannot run them.
    std::string out, err;

    try {
        if(jlib::sys::run({ "file", "--version" }, out, err) != 0) {
            std::cerr << "file(1) does not understand --version; skipping\n";
            return 77;
        }
    }
    catch(std::exception& e) {
        std::cerr << "no file(1): " << e.what() << "; skipping\n";
        return 77;
    }

    arguments_are_arguments();
    a_filename_that_is_a_command();
    identifying_a_file();

    // What a green run does NOT establish.
    //
    // Not that jlib no longer runs a shell.  sys::shell() is still there and
    // still hands its argument to /bin/sh, which is what it is for; what
    // changed is that nothing in the library calls it any more.  A future
    // caller that concatenates a string into it is the same bug again, and no
    // test can see that coming.
    //
    // Not that MimeType is right about any particular file.  It asks file(1)
    // and repeats the answer, so what it says depends on the version of
    // file(1) installed and on its magic database.  The two assertions above
    // are about which input decides -- the content, not the name -- and not
    // about the specific strings.
    //
    // Not Windows.  fork() and execvp() are POSIX, and so is everything else
    // in jlib::sys.
    return failures ? 1 : 0;
}
