/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2002 Joey Yandle <xoloki@gmail.com>
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
/*
 * net_http_files_test -- serving a directory, and refusing to serve anything else.
 *
 * Most of this file is the second half.  Handing out a file is a dozen lines;
 * being sure that is the only thing handed out is the work, and it is the
 * first feature in this server with a way to be wrong that matters.
 *
 * Every refusal here is a 404 rather than a 403, deliberately, so the
 * assertions are about status codes that all look alike.  What distinguishes
 * them is what was asked for -- a traversal, a symlink out, a directory -- and
 * each is arranged on disk rather than described.
 */

#include <jlib/net/http.hh>
#include <jlib/net/http_server.hh>
#include <jlib/util/URL.hh>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>

#include <sys/stat.h>
#include <unistd.h>

using namespace jlib;
using namespace jlib::net;

static int failures = 0;

// One value, used by the registration and by every assertion about it, so a
// test cannot agree with itself about a string neither end actually sent.
static const char* const CACHE = "max-age=31536000, immutable";

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static void write_file(const std::string& path, const std::string& text) {
    std::ofstream o(path.c_str(), std::ios::binary);

    o << text;
}

/** A root to serve, and a sibling nobody should ever reach. */
struct tree {
    std::string base;
    std::string root;
    std::string secret;
    std::size_t big_size = 0;
    std::string big_text;

    tree() {
        char pattern[] = "/tmp/jlib_files_XXXXXX";

        base = ::mkdtemp(pattern);
        root = base + "/www";
        secret = base + "/secret.txt";

        ::mkdir(root.c_str(), 0755);
        ::mkdir((root + "/sub").c_str(), 0755);

        write_file(root + "/index.html", "<h1>hello</h1>\n");
        write_file(root + "/app.js", "console.log(1)\n");
        write_file(root + "/sub/deep.txt", "deep\n");
        write_file(root + "/nosuffix", "no extension here\n");

        // Larger than one read block, so serving it has to loop -- and
        // deliberately not a multiple of it, so the last read is short.
        std::string big;

        big.reserve(300000);

        for(int i = 0; big.size() < 300000; i++)
            big += "line " + std::to_string(i) + " of a file that does not fit in one block\n";

        write_file(root + "/big.txt", big);

        big_size = big.size();
        big_text = big;
        write_file(secret, "you should never see this\n");

        // Two symlinks: one that stays inside, one that leaves.
        ::symlink((root + "/index.html").c_str(), (root + "/inside.html").c_str());
        ::symlink(secret.c_str(), (root + "/escape.txt").c_str());

        // Things inside the root that are not files.  Both matter because the
        // server now opens before it knows what it opened: a FIFO with no
        // writer blocks an ordinary open forever, and a character device has
        // no end.  Neither was reachable when the type was checked first.
        ::mkfifo((root + "/hose.txt").c_str(), 0644);
        ::symlink("/dev/zero", (root + "/zero.txt").c_str());
    }

    ~tree() {
        // Best effort; these are under /tmp and small.
        const std::string cmd = "rm -rf '" + base + "'";

        if(::system(cmd.c_str()) != 0) { /* nothing useful to do */ }
    }
};

/**
 * Stop and join a server however the block is left.
 *
 * Without this, an assertion path that throws -- which is what a *broken*
 * server does to a client, not what a working one does -- skips the stop()
 * and join() below it, and ~thread on a joinable thread calls terminate.
 * The test then dies with no output at all.
 *
 * That is not hypothetical: three of the four breaks written against this file
 * aborted at exit 134 with zero FAIL lines, and a harness grepping for "FAIL"
 * read that as success.  A test whose failure mode is silence is worse than no
 * test, so the thread is joined on the way out whatever happened.
 */
struct running {
    http::server& s;
    std::thread th;

    explicit running(http::server& server)
        : s(server), th([&server]{ server.run(); }) {}

    ~running() {
        s.stop();

        if(th.joinable()) th.join();
    }

    running(const running&) = delete;
    running& operator=(const running&) = delete;
};

/**
 * Ask, and turn a refusal into an answer rather than an escape.
 *
 * A malformed response makes the client throw -- correctly; that is what the
 * framing work was for.  But a *test* wants the assertion below it to fail
 * with a name, not the run to end here, so a throw becomes status 0 and every
 * assertion then says which expectation it was.
 */
/** The first line of a body, for a detail string. */
static std::string first_line(const std::string& body) {
    const std::string::size_type nl = body.find('\n');

    return nl == std::string::npos ? body.substr(0, 60)
                                   : body.substr(0, nl > 60 ? 60 : nl);
}

static util::http::Response ask(http::server& s, const std::string& method,
                                const std::string& path)
{
    try { return http::request(method, util::URL(s.url(path))); }
    catch(std::exception&) { return util::http::Response(); }
}

static util::http::Response ask_with(http::server& s, const std::string& method,
                                     const std::string& path,
                                     const std::string& name,
                                     const std::string& value)
{
    util::http::fields f;

    f.add(name, value);

    try { return http::request(method, util::URL(s.url(path)), f); }
    catch(std::exception&) { return util::http::Response(); }
}

static void what_it_serves(http::server& s) {
    std::cout << "\nwhat it serves:\n";

    {
        const util::http::Response r = ask(s, "GET", "/static/index.html");

        ok("  a file under the root", r.status() == 200 &&
           r.body() == "<h1>hello</h1>\n", r.body());

        ok("  with a type from its extension",
           util::http::fold(r.fields().get("Content-Type"))
               .find("text/html") == 0, r.fields().get("Content-Type"));

        ok("  and validators the server made itself",
           r.fields().has("ETag") && r.fields().has("Last-Modified"),
           r.fields().get("ETag") + " / " + r.fields().get("Last-Modified"));

        // **Weak, and it says so.**  mtime and size cannot promise the bytes
        // are identical, only that they are probably the same file.
        ok("  the ETag is weak, because mtime and size cannot promise more",
           r.fields().get("ETag").compare(0, 2, "W/") == 0,
           r.fields().get("ETag"));
    }

    {
        const util::http::Response r = ask(s, "GET", "/static/sub/deep.txt");

        ok("  a file further down", r.status() == 200 && r.body() == "deep\n",
           r.body());
    }

    {
        const util::http::Response r = ask(s, "GET", "/static/app.js");

        ok("  javascript by extension, not by sniffing its contents",
           util::http::fold(r.fields().get("Content-Type"))
               .find("application/javascript") == 0,
           r.fields().get("Content-Type"));
    }

    {
        const util::http::Response r = ask(s, "GET", "/static/nosuffix");

        // The one answer that cannot be wrong dangerously: a browser will not
        // execute an octet-stream.
        ok("  and no extension means octet-stream",
           r.fields().get("Content-Type") == "application/octet-stream",
           r.fields().get("Content-Type"));
    }

    {
        const util::http::Response r = ask(s, "GET", "/static/inside.html");

        ok("  a symlink that stays inside the root is fine",
           r.status() == 200 && r.body() == "<h1>hello</h1>\n",
           std::to_string(r.status()));
    }
}

static void what_it_refuses(http::server& s, const tree& t) {
    std::cout << "\nwhat it refuses, all as 404:\n";

    struct { const char* path; const char* why; } cases[] = {
        { "/static/../secret.txt",          "a traversal" },
        { "/static/sub/../../secret.txt",   "a traversal from further down" },
        { "/static/escape.txt",             "a symlink pointing out of the root" },
        { "/static/nope.txt",               "something that is not there" }
    };

    // A directory and the prefix itself used to be here, as 404s.  They are
    // not refusals any more -- a directory is served by its index and, asked
    // for without a trailing slash, is a 301.  Moved to their own section
    // rather than deleted, because what they were really asserting -- that
    // nothing outside the root comes back -- still holds and is still worth
    // asserting.
    //
    // This is a deliberate change to what files() answers, and the only one
    // in this branch: anything relying on a directory being a 404 sees a 200
    // or a 301 now.

    for(std::size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const util::http::Response r = ask(s, "GET", cases[i].path);

        ok(std::string("  ") + cases[i].why, r.status() == 404,
           std::string(cases[i].path) + " -> " + std::to_string(r.status()));

        // The stronger half: whatever the status, the secret is not in it.
        ok(std::string("  ") + cases[i].why + ", and says nothing about it",
           r.body().find("never see this") == std::string::npos,
           r.body().substr(0, 40));
    }

    (void)t;
}

static void the_conditional_and_head_paths(http::server& s) {
    std::cout << "\nwhat the rest of the server already does for it:\n";

    const util::http::Response first = ask(s, "GET", "/static/index.html");

    {
        // Nothing in files() implements this: the ETag it produced is enough.
        const util::http::Response r =
            ask_with(s, "GET", "/static/index.html", "If-None-Match",
                     first.fields().get("ETag"));

        ok("  the ETag it handed out comes back as a 304", r.status() == 304,
           std::to_string(r.status()));
    }

    {
        const util::http::Response r =
            ask_with(s, "GET", "/static/index.html", "If-Modified-Since",
                     first.fields().get("Last-Modified"));

        ok("  and so does the Last-Modified", r.status() == 304,
           std::to_string(r.status()));
    }

    {
        const util::http::Response r = ask(s, "HEAD", "/static/index.html");

        ok("  HEAD works without files() knowing about it",
           r.status() == 200 && r.body().empty(),
           std::to_string(r.status()));

        ok("  with the length the body would have had",
           r.fields().get("Content-Length") ==
           std::to_string(first.body().size()),
           r.fields().get("Content-Length"));
    }
}

/**
 * A file larger than one block, and how it is framed.
 *
 * The body is read a block at a time rather than whole, so what a request
 * costs is the block and not the file.  **That is structural and this cannot
 * assert it** -- nothing here counts allocations.  What it can assert is that
 * the looping works: a file several blocks long, not a multiple of the block,
 * arrives byte for byte.
 *
 * The framing is assertable and worth pinning.  A streamed response normally
 * has no length and ends at the chunked terminator; a file is the case where
 * stat() knows the length in advance, so it is sent as a counted body -- which
 * is better on both ends and is what lets the connection be reused without
 * chunking.
 */
static void a_file_bigger_than_a_block(http::server& s, const tree& t) {
    std::cout << "\na file larger than one read block:\n";

    const util::http::Response r = ask(s, "GET", "/static/big.txt");

    ok("  it is served", r.status() == 200, std::to_string(r.status()));

    ok("  every byte of it, in order",
       r.body().size() == t.big_size && r.body() == t.big_text,
       std::to_string(r.body().size()) + " of " + std::to_string(t.big_size));

    ok("  counted, because stat knew the length before anything was read",
       r.fields().get("Content-Length") == std::to_string(t.big_size),
       r.fields().get("Content-Length"));

    ok("  and not chunked, which would frame it a second time",
       !r.fields().has("Transfer-Encoding"),
       r.fields().get("Transfer-Encoding"));
}

/**
 * Byte ranges: what is answered, what is refused, and what is ignored.
 *
 * The arithmetic is inclusive on both ends -- `bytes=0-0` is one octet, not
 * none -- and the three forms mean different things, so each is asked for
 * against a file whose contents are known exactly.
 *
 * The ignoring is as much a decision as the answering.  RFC 9110 14.2 lets a
 * server ignore a Range outright, and this one does for a unit it does not
 * know and for more than one range at a time; both come back as the whole
 * file, not as an error.
 */
static void byte_ranges(http::server& s, const tree& t) {
    std::cout << "\nbyte ranges:\n";

    const std::string& all = t.big_text;
    const long long size = (long long)t.big_size;

    {
        const util::http::Response r = ask(s, "GET", "/static/big.txt");

        ok("  a full response advertises that ranges are possible",
           util::http::fold(r.fields().get("Accept-Ranges")) == "bytes",
           r.fields().get("Accept-Ranges"));
    }

    {
        const util::http::Response r =
            ask_with(s, "GET", "/static/big.txt", "Range", "bytes=200-999");

        ok("  a middle range is 206", r.status() == 206,
           std::to_string(r.status()));

        ok("  with exactly those octets",
           r.body() == all.substr(200, 800), std::to_string(r.body().size()));

        std::ostringstream want;

        want << "bytes 200-999/" << size;

        ok("  and a Content-Range saying which, of how many",
           r.fields().get("Content-Range") == want.str(),
           r.fields().get("Content-Range"));

        ok("  the length is the range, not the file",
           r.fields().get("Content-Length") == "800",
           r.fields().get("Content-Length"));
    }

    {
        // Inclusive on both ends: one octet, not zero.
        const util::http::Response r =
            ask_with(s, "GET", "/static/big.txt", "Range", "bytes=0-0");

        ok("  bytes=0-0 is one octet, because both ends are inclusive",
           r.status() == 206 && r.body() == all.substr(0, 1),
           std::to_string(r.body().size()));
    }

    {
        const util::http::Response r =
            ask_with(s, "GET", "/static/big.txt", "Range", "bytes=-500");

        ok("  a suffix range is the last N octets",
           r.status() == 206 && r.body() == all.substr(all.size() - 500),
           std::to_string(r.body().size()));
    }

    {
        std::ostringstream ask_from;

        ask_from << "bytes=" << (size - 10) << "-";

        const util::http::Response r =
            ask_with(s, "GET", "/static/big.txt", "Range", ask_from.str());

        ok("  an open-ended range runs to the end",
           r.status() == 206 && r.body() == all.substr(all.size() - 10),
           std::to_string(r.body().size()));
    }

    {
        // Past the end is clamped, not refused -- 14.1.2.
        std::ostringstream past;

        past << "bytes=" << (size - 5) << "-" << (size + 1000);

        const util::http::Response r =
            ask_with(s, "GET", "/static/big.txt", "Range", past.str());

        ok("  a last-pos past the end is clamped to it",
           r.status() == 206 && r.body() == all.substr(all.size() - 5),
           std::to_string(r.body().size()));
    }

    {
        // A first-pos past the end is a different thing: the client's offset
        // is wrong and it needs to be told, not quietly handed everything.
        std::ostringstream beyond;

        beyond << "bytes=" << (size + 100) << "-";

        const util::http::Response r =
            ask_with(s, "GET", "/static/big.txt", "Range", beyond.str());

        ok("  a first-pos past the end is 416, not the whole file",
           r.status() == 416, std::to_string(r.status()));

        std::ostringstream want;

        want << "bytes */" << size;

        ok("  with the length, so the next ask can be right",
           r.fields().get("Content-Range") == want.str(),
           r.fields().get("Content-Range"));
    }

    {
        const util::http::Response r =
            ask_with(s, "GET", "/static/big.txt", "Range", "bytes=-0");

        ok("  a zero-length suffix is 416 rather than an empty 206",
           r.status() == 416, std::to_string(r.status()));
    }

    {
        const util::http::Response r =
            ask_with(s, "GET", "/static/big.txt", "Range", "items=1-2");

        ok("  a unit that is not bytes is ignored, giving the whole file",
           r.status() == 200 && r.body().size() == t.big_size,
           std::to_string(r.status()));
    }

    {
        // Answering two means multipart/byteranges, which this declines to
        // implement; 14.2 permits ignoring the field entirely.
        const util::http::Response r =
            ask_with(s, "GET", "/static/big.txt", "Range", "bytes=0-99,200-299");

        ok("  more than one range is ignored, giving the whole file",
           r.status() == 200 && r.body().size() == t.big_size,
           std::to_string(r.status()));
    }

    {
        const util::http::Response r =
            ask_with(s, "GET", "/static/big.txt", "Range", "bytes=nonsense");

        ok("  and so is anything the grammar refuses",
           r.status() == 200 && r.body().size() == t.big_size,
           std::to_string(r.status()));
    }
}

/**
 * If-Range, and the honest validator that cannot satisfy it.
 *
 * `files()` sends a weak ETag on purpose -- mtime at second resolution plus a
 * size cannot promise the bytes are identical -- and a weak tag can never pass
 * the strong comparison If-Range requires (13.1.5).  So a conditional range
 * against a served file comes back whole.
 *
 * **That is the intended answer, not a gap.**  Splicing a piece of one
 * representation onto a client's copy of another is the failure If-Range
 * exists to prevent, and a validator that cannot rule it out should not be
 * used to authorise it.  The cost is a re-read; the alternative is corruption.
 */
static void if_range_against_a_weak_validator(http::server& s, const tree& t) {
    std::cout << "\nIf-Range against the weak validator files() sends:\n";

    const util::http::Response first = ask(s, "GET", "/static/big.txt");

    ok("  the validator is weak", first.fields().get("ETag")
       .compare(0, 2, "W/") == 0, first.fields().get("ETag"));

    {
        util::http::fields f;

        f.add("Range", "bytes=0-99");
        f.add("If-Range", first.fields().get("ETag"));

        util::http::Response r;

        try { r = http::request("GET", util::URL(s.url("/static/big.txt")), f); }
        catch(std::exception&) {}

        ok("  a weak tag cannot authorise a range, so the whole file comes back",
           r.status() == 200 && r.body().size() == t.big_size,
           std::to_string(r.status()) + " " + std::to_string(r.body().size()));
    }

    {
        // A date matches exactly, and Last-Modified is what a client holding a
        // weak tag will have used instead.
        util::http::fields f;

        f.add("Range", "bytes=0-99");
        f.add("If-Range", first.fields().get("Last-Modified"));

        util::http::Response r;

        try { r = http::request("GET", util::URL(s.url("/static/big.txt")), f); }
        catch(std::exception&) {}

        ok("  a matching Last-Modified does authorise one",
           r.status() == 206 && r.body() == t.big_text.substr(0, 100),
           std::to_string(r.status()) + " " + std::to_string(r.body().size()));
    }

    {
        util::http::fields f;

        f.add("Range", "bytes=0-99");
        f.add("If-Range", "Sun, 06 Nov 1994 08:49:37 GMT");

        util::http::Response r;

        try { r = http::request("GET", util::URL(s.url("/static/big.txt")), f); }
        catch(std::exception&) {}

        ok("  and one that does not match gives the whole file back",
           r.status() == 200 && r.body().size() == t.big_size,
           std::to_string(r.status()));
    }
}

/**
 * Freshness, which is the half of caching the validators do not cover.
 *
 * An ETag says *whether* a copy is still good and costs a round trip to find
 * out.  `Cache-Control` says *how long* not to ask, which is the part that
 * makes the second visit free.  Both routes below serve the same tree, so any
 * difference between them is the registration and nothing else.
 */
/**
 * Things inside the root that are not regular files.
 *
 * `locate()` opens before it can know what it opened -- that is the whole
 * point of #234, since a name checked and then opened is a name checked twice
 * with a gap in between.  The cost is that the open itself has to survive
 * whatever is there, and the two that do not survive a plain `O_RDONLY` are a
 * FIFO with no writer, which blocks until somebody writes, and a character
 * device, which never ends.
 *
 * **A failure here is a hang, not a FAIL line.**  If the open stops being
 * non-blocking this function never returns and the suite times out with no
 * assertion having failed -- which is the shape of failure that has fooled a
 * break harness in this tree before, so the harness for this one uses a
 * watchdog rather than reading exit status alone.
 */
/**
 * The race in #234, run rather than reasoned about.
 *
 * A symlink inside the root is flipped between a file that is inside and the
 * secret that is outside, atomically, while requests for it are in flight.
 * The old order -- resolve the name, check it, then open the name again --
 * could resolve to the inside file, be flipped, and then open the secret.
 *
 * **The assertion is one-sided, which is what makes it safe to run.**  It does
 * not require the window to be hit; it requires that the secret never comes
 * back.  Correct code passes whether or not the timing lands, so this cannot
 * flake -- it can only fail for code that is actually wrong, and then only
 * some of the time, which is why the iteration count is large.  200 and 404
 * are both fine answers here, and both occur: a flip to the secret is refused
 * and a flip back is served.
 *
 * `rename()` rather than unlink-then-symlink so the name is never briefly
 * absent -- otherwise most requests would land in the gap and get an honest
 * 404 without ever reaching the window being tested.
 */
static void a_symlink_that_changes_while_it_is_served(http::server& s,
                                                      const tree& t)
{
    std::cout << "\na symlink flipped under a request in flight:\n";

    const std::string flip = t.root + "/flip.txt";
    const std::string spare = t.root + "/.flip.tmp";

    ::symlink((t.root + "/index.html").c_str(), flip.c_str());

    std::atomic<bool> stop(false);

    std::thread flipper([&] {
        bool out = false;

        while(!stop) {
            ::unlink(spare.c_str());
            ::symlink(out ? t.secret.c_str() : (t.root + "/index.html").c_str(),
                      spare.c_str());
            ::rename(spare.c_str(), flip.c_str());

            out = !out;
        }
    });

    int leaked = 0;
    int served = 0;
    int refused = 0;

    std::string first;

    for(int i = 0; i < 400; i++) {
        const util::http::Response r = ask(s, "GET", "/static/flip.txt");

        // **Anything but the inside file, with a 200 on it, is a leak.**
        //
        // Searching the body for the secret's text is what this did first, and
        // it could not fail: the old order takes the length from its `stat` of
        // the *inside* file and the bytes from whatever the name opened, so a
        // leaked secret arrives truncated to fifteen octets -- "you should
        // neve" -- and the phrase being searched for is cut off mid-way.  The
        // wrong Content-Length is the same defect as the wrong file, so the
        // test that sees both is the one that compares the whole body.
        if(r.status() == 200) {
            if(r.body() == "<h1>hello</h1>\n") served++;
            else { leaked++; if(leaked == 1) first = r.body(); }
        }
        else refused++;
    }

    stop = true;
    flipper.join();

    ::unlink(flip.c_str());
    ::unlink(spare.c_str());

    ok("  nothing but the inside file is ever served, however the timing lands",
       leaked == 0,
       std::to_string(leaked) + " leaked" +
       (first.empty() ? "" : " (first: \"" + first + "\")") + ", " +
       std::to_string(served) + " served, " + std::to_string(refused) +
       " refused");

    // Without this the test could pass by never reaching the server at all --
    // a mistyped path answers 404 four hundred times and leaks nothing.
    ok("  and the route was live throughout", served > 0,
       std::to_string(served));
}

static void things_that_are_not_files(http::server& s) {
    std::cout << "\ninside the root but not a file:\n";

    {
        const util::http::Response r = ask(s, "GET", "/static/hose.txt");

        ok("  a FIFO nobody is writing to is refused, and does not hang",
           r.status() == 404, std::to_string(r.status()));
    }

    {
        const util::http::Response r = ask(s, "GET", "/static/zero.txt");

        ok("  and a character device is refused rather than streamed forever",
           r.status() == 404, std::to_string(r.status()));
    }
}

static void freshness(http::server& s) {
    std::cout << "\nfreshness:\n";

    {
        const util::http::Response r = ask(s, "GET", "/cached/index.html");

        ok("  a route registered with one sends it",
           r.status() == 200 && r.fields().get("Cache-Control") == CACHE,
           r.fields().get("Cache-Control"));
    }

    {
        const util::http::Response r = ask(s, "GET", "/static/index.html");

        // The server invents no freshness of its own.  A caller who says
        // nothing gets nothing, and the client falls back on its own
        // heuristics -- which is its business rather than this server's guess.
        ok("  a route registered without one sends nothing",
           r.status() == 200 && r.fields().get("Cache-Control").empty(),
           r.fields().get("Cache-Control"));
    }

    {
        const util::http::Response r =
            ask_with(s, "GET", "/cached/index.html", "Range", "bytes=0-3");

        ok("  a 206 carries it too", r.status() == 206 &&
           r.fields().get("Cache-Control") == CACHE,
           std::to_string(r.status()) + " " + r.fields().get("Cache-Control"));
    }

    {
        const util::http::Response first = ask(s, "GET", "/cached/index.html");
        const util::http::Response r =
            ask_with(s, "GET", "/cached/index.html", "If-None-Match",
                     first.fields().get("ETag"));

        // RFC 9110 15.4.5.  The 304 path builds its answer from a different
        // response object than the 200 path, so this is a separate place that
        // has to remember -- and the cost of forgetting is a client that
        // revalidates every time despite having been told for a year.
        ok("  and so does the 304, per 9110 15.4.5", r.status() == 304 &&
           r.fields().get("Cache-Control") == CACHE,
           std::to_string(r.status()) + " " + r.fields().get("Cache-Control"));
    }

    {
        const util::http::Response r = ask(s, "HEAD", "/cached/index.html");

        ok("  and a HEAD, which is the 200 without the body",
           r.status() == 200 && r.fields().get("Cache-Control") == CACHE,
           r.fields().get("Cache-Control"));
    }
}

static void a_root_that_is_not_there() {
    std::cout << "\na root that cannot be resolved:\n";

    http::server s(0, "127.0.0.1");

    bool threw = false;
    std::string why;

    try { s.files("/static/*", "/no/such/directory/anywhere"); }
    catch(std::exception& e) { threw = true; why = e.what(); }

    // At registration, where the caller is -- not as a 404 per request
    // forever, which is what not checking would look like.
    ok("  is refused when the route is registered", threw, why);
}

static void a_cache_control_that_is_not_one(const tree& t) {
    std::cout << "\na Cache-Control value that would not survive the wire:\n";

    http::server s(0, "127.0.0.1");

    bool threw = false;
    std::string why;

    // A newline in a field value is header injection if it ever reaches a
    // socket.  The serialiser would refuse it -- but on every response rather
    // than here, with no caller left to point at.
    try { s.files("/bad/*", t.root, "max-age=60\r\nX-Injected: yes"); }
    catch(std::exception& e) { threw = true; why = e.what(); }

    ok("  is refused when the route is registered", threw, why);

    bool empty_threw = false;

    // Empty is not an error, it is the way to ask for no directive -- and it
    // is the default, so a throw here would break every existing caller.
    try { s.files("/fine/*", t.root, ""); }
    catch(std::exception&) { empty_threw = true; }

    ok("  but empty is how you ask for none, and is fine", !empty_threw);
}


/**
 * A directory is served by its index, and asked for without a slash it moves.
 *
 * Apache's mod_dir, which is enabled globally in `mods-enabled/dir.conf` and
 * therefore never mentioned in a vhost -- which is exactly why it is easy to
 * miss when translating one, and why `GET /` returning 404 looked like a small
 * caveat rather than a site that does not work.
 */
static void a_directory_is_served_by_its_index(http::server& s, const tree& t) {
    std::cout << "\ndirectory index:\n";

    {
        const util::http::Response r = ask(s, "GET", "/static/");

        ok("  a directory serves its index", r.status() == 200,
           std::to_string(r.status()));
        ok("  which is the index file's content",
           r.body().find("<h1>hello</h1>") != std::string::npos,
           first_line(r.body()));
    }

    {
        // The prefix itself, which is the case every link to a site root is.
        const util::http::Response r = ask(s, "GET", "/static");

        ok("  the prefix without a slash is moved", r.status() == 301,
           std::to_string(r.status()));
        ok("  to itself with one",
           r.fields().get("Location") == "/static/",
           r.fields().get("Location"));
    }

    {
        const util::http::Response r = ask(s, "GET", "/static/sub");

        ok("  and so is a subdirectory", r.status() == 301,
           std::to_string(r.status()));

        // **The redirect that must not loop.**  `rest` has been through
        // tidy_path by the time the file layer sees it, so "sub/" and "sub"
        // arrive identical -- deciding from `rest` sent "/static/sub/" to
        // "/static/sub//" and onwards forever.  Asserting the *next* request
        // is what catches that; asserting only this one does not.
        ok("  to itself with one",
           r.fields().get("Location") == "/static/sub/",
           r.fields().get("Location"));
    }

    {
        const util::http::Response r = ask(s, "GET", "/static/sub/");

        ok("  and following that redirect does not redirect again",
           r.status() == 404 || r.status() == 200,
           std::to_string(r.status()) + " " + r.fields().get("Location"));

        // sub/ has no index in this tree, so the honest answer is 404.  Not a
        // listing: publishing what is in a directory is a different feature
        // and one nobody asked for.
        ok("  a directory with no index is 404, not a listing",
           r.status() == 404, std::to_string(r.status()));
        ok("  and says nothing about what is in it",
           r.body().find("deep.txt") == std::string::npos, first_line(r.body()));
    }

    {
        // The query belongs to the request, not to the Location: the client
        // puts it back.  Carrying it would reflect chosen bytes into a header.
        const util::http::Response r = ask(s, "GET", "/static/sub?a=1&b=2");

        ok("  a query is not carried into the Location",
           r.fields().get("Location") == "/static/sub/",
           r.fields().get("Location"));
    }

    {
        // Break-the-guard: a file must not have become a redirect.
        const util::http::Response r = ask(s, "GET", "/static/app.js");

        ok("  an ordinary file is still served, not moved", r.status() == 200,
           std::to_string(r.status()));
    }

    {
        // And the index must not have become a way past containment: the
        // index file is resolved by calling locate() again, so it inherits
        // every check.  A traversal that named a directory outside the root
        // still fails before any index is looked for.
        const util::http::Response r = ask(s, "GET", "/static/../");

        ok("  a traversal that names a directory is still refused",
           r.status() == 404 || r.status() == 301,
           std::to_string(r.status()) + " " + r.fields().get("Location"));
        ok("  and serves nothing from outside the root",
           r.body().find("never see this") == std::string::npos,
           first_line(r.body()));
    }

    (void)t;
}

int main() {
    std::cout << "net_http_files_test\n";

    try {
        tree t;

        {
            http::server s(0, "127.0.0.1");

            s.files("/static/*", t.root);
            s.files("/cached/*", t.root, CACHE);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            running go(s);

            std::cout << "\n-- the blocking server --\n";
            what_it_serves(s);
            what_it_refuses(s, t);
            the_conditional_and_head_paths(s);
            a_file_bigger_than_a_block(s, t);
            byte_ranges(s, t);
            if_range_against_a_weak_validator(s, t);
            freshness(s);
            things_that_are_not_files(s);
            a_symlink_that_changes_while_it_is_served(s, t);
            a_directory_is_served_by_its_index(s, t);
        }

        {
            http::server s(http::server::async_t(), 0, "127.0.0.1");

            s.files("/static/*", t.root);
            s.files("/cached/*", t.root, CACHE);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            running go(s);

            std::cout << "\n-- the async server --\n";
            what_it_serves(s);
            what_it_refuses(s, t);
            the_conditional_and_head_paths(s);
            a_file_bigger_than_a_block(s, t);
            byte_ranges(s, t);
            if_range_against_a_weak_validator(s, t);
            freshness(s);
            things_that_are_not_files(s);
            a_symlink_that_changes_while_it_is_served(s, t);
            a_directory_is_served_by_its_index(s, t);
        }

        a_root_that_is_not_there();
        a_cache_control_that_is_not_one(t);
    }
    catch(std::exception& e) {
        std::cerr << "net_http_files_test: " << e.what() << "\n";
        return 1;
    }

    if(failures) {
        std::cerr << "net_http_files_test: " << failures << " failed\n";
        return 1;
    }

    std::cout << "net_http_files_test: all good\n";

    return 0;
}
