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

/**
 * jlib::util::file::kill() and keep(), which had no test at all.
 *
 * They rewrite a file in place -- MFolder and net.cc delete messages from an
 * mbox with kill() -- and they did it by handing `cat <tmp> > <path>` to
 * /bin/sh with both paths unquoted.  So the sections that matter here are the
 * two that have nothing to do with slicing: a path containing a **space**, and
 * a path containing a **shell metacharacter**.  Both are ordinary things for a
 * mail folder to be called, both silently did the wrong thing, and the second
 * one ran a command.
 *
 * Every assertion below fails on the old implementation except the first two.
 */

#include <jlib/util/util.hh>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static const char* const CONTENT = "0123456789";

/** Write CONTENT to path, creating parent directories as needed. */
static void seed(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);

    out << CONTENT;
    out.close();
}

static std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);

    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/** kill() the points from a freshly seeded path and return what is left. */
static std::string killed(const std::filesystem::path& path,
                          std::vector<long> pts) {
    seed(path);
    jlib::util::file::kill(path.string(), pts);

    return slurp(path);
}

static std::string kept(const std::filesystem::path& path,
                        std::vector<long> pts) {
    seed(path);
    jlib::util::file::keep(path.string(), pts);

    return slurp(path);
}

int main() {
    const std::filesystem::path dir = "util_file_test.d";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    // The file the injection section looks for, cleared before rather than
    // after: the assertion is that no command ran *in this run*, so a leftover
    // from an earlier one would report someone else's injection as this one's
    // -- and would make a real regression indistinguishable from debris. The
    // shell would create it in the working directory, which is where the
    // driver runs the test from.
    const std::filesystem::path injected = "injected.txt";

    std::filesystem::remove(injected, ec);

    const std::filesystem::path plain = dir / "plain";

    std::cout << "\nwhat kill() and keep() do:\n";

    // "234" is bytes [2,5), the region the pair marks.
    ok("  kill removes the region a pair marks",
       killed(plain, {2, 5}) == "0156789", killed(plain, {2, 5}));

    // An odd count means the last point runs to the end of the file.
    ok("  an odd point kills through to the end",
       killed(plain, {2}) == "01", killed(plain, {2}));

    ok("  keep keeps the region a pair marks",
       kept(plain, {2, 5}) == "234", kept(plain, {2, 5}));

    ok("  an odd point keeps through to the end",
       kept(plain, {5}) == "56789", kept(plain, {5}));

    // Killing from zero empties the file.  Worth its own assertion because the
    // obvious way to write the copy -- `out << in.rdbuf()` -- sets failbit when
    // it inserts no characters, so an empty result would have been reported as
    // an error by an implementation that looked correct.
    ok("  killing from zero leaves an empty file, and does not throw",
       killed(plain, {0}).empty(), "\""+killed(plain, {0})+"\"");

    std::cout << "\nthe part that was broken -- a path /bin/sh would mangle:\n";

    // THE LOAD-BEARING ASSERTION.  `cat <tmp> > /…/with space/mbox` redirects
    // to "/…/with", so the rebuilt file landed under a different name and the
    // real one was never touched -- a deleted message came back.  Ordinary on
    // macOS, where a mail folder lives under a path with a space in it.
    const std::filesystem::path spaced = dir / "with space" / "mbox";

    ok("  a space in the path still rewrites the file",
       killed(spaced, {2, 5}) == "0156789", killed(spaced, {2, 5}));

    // And the file the shell would have written instead must not exist.
    ok("  and nothing was written beside it",
       !std::filesystem::exists(dir / "with"));

    // A folder name reaches net.cc's caller from a server or the user, so this
    // is reachable rather than theoretical: the redirect ended at "semi" and
    // the rest ran as a command.
    const std::filesystem::path meta = dir / "semi;touch injected.txt";

    ok("  a shell metacharacter in the path still rewrites the file",
       killed(meta, {2, 5}) == "0156789", killed(meta, {2, 5}));

    ok("  and no command ran",
       !std::filesystem::exists(injected)
       && !std::filesystem::exists(dir / injected));

    std::cout << "\nand a failure is no longer silent:\n";

    // The ignored return value was the original complaint, so a write that
    // cannot happen has to be an error rather than a successful no-op.  A
    // read-only file is the portable way to make the open fail; a directory
    // would be the obvious one and cannot be used, because kill() hangs on a
    // path it cannot read (#391) rather than reaching the write at all.
    if(geteuid() == 0) {
        std::cout << "  n/a    running as root, where a mode bit cannot make a"
                     " write fail\n";
    }
    else {
        const std::filesystem::path ro = dir / "read-only";

        seed(ro);
        std::filesystem::permissions(ro, std::filesystem::perms::owner_read,
                                     std::filesystem::perm_options::replace);

        bool threw = false;

        try {
            std::vector<long> pts{2, 5};

            jlib::util::file::kill(ro.string(), pts);
        }
        catch(const std::exception&) {
            threw = true;
        }

        ok("  a path that cannot be written throws", threw);

        // Writable again, or remove_all cannot clean up after itself.
        std::filesystem::permissions(ro, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
    }

    std::filesystem::remove_all(dir, ec);

    // What a green run does not establish.
    //
    // Nothing here is concurrent, and these functions are not safe to call on
    // a file another process is appending to -- the rebuilt copy is written
    // from a snapshot of the size, so anything that arrives mid-rewrite is
    // lost.  That was true before this change and still is; it is a property of
    // rewriting a mailbox in place, not of how the bytes get copied.
    //
    // The write is not atomic either.  The old code truncated the target
    // through the shell and the new code truncates it directly, so an
    // interrupted kill() still leaves a half-written mbox.  Writing the temp
    // file and rename(2)-ing it over the target would fix that, and would be a
    // different change with a different argument -- the point here is that the
    // failure is now reported rather than that it cannot happen.
    //
    // kill() on a path that does not exist still creates an empty file rather
    // than throwing, which is what the shell did too.  Unchanged, untested,
    // and arguably wrong.
    //
    // And nothing here covers a path that cannot be *read*, because kill()
    // does not fail on one -- it hangs, in jlib::sys::read, which spins on a
    // stream that is fail() but not eof().  That is #391 and a separate defect
    // from anything this file guards; the first draft of the section above used
    // a directory for the unwritable path and hung the suite for ten minutes.
    //
    // The root run skips the throw assertion rather than asserting something
    // weaker, so on the container this file proves the slicing and the quoting
    // and says nothing about the error path.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
