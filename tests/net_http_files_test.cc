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

#include <sys/stat.h>
#include <unistd.h>

using namespace jlib;
using namespace jlib::net;

static int failures = 0;

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
        { "/static/sub",                    "a directory" },
        { "/static/",                       "the root itself" },
        { "/static/nope.txt",               "something that is not there" }
    };

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

int main() {
    std::cout << "net_http_files_test\n";

    try {
        tree t;

        {
            http::server s(0, "127.0.0.1");

            s.files("/static/*", t.root);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            running go(s);

            std::cout << "\n-- the blocking server --\n";
            what_it_serves(s);
            what_it_refuses(s, t);
            the_conditional_and_head_paths(s);
            a_file_bigger_than_a_block(s, t);
            byte_ranges(s, t);
            if_range_against_a_weak_validator(s, t);
        }

        {
            http::server s(http::server::async_t(), 0, "127.0.0.1");

            s.files("/static/*", t.root);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            running go(s);

            std::cout << "\n-- the async server --\n";
            what_it_serves(s);
            what_it_refuses(s, t);
            the_conditional_and_head_paths(s);
            a_file_bigger_than_a_block(s, t);
            byte_ranges(s, t);
            if_range_against_a_weak_validator(s, t);
        }

        a_root_that_is_not_there();
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
