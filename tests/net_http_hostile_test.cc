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

/**
 * Hostile input: what #240 found by sending it.
 *
 * Raw bytes rather than net::http::request, because a correct client cannot
 * send most of what is below -- that is the point of sending it.
 *
 * Everything here runs against both servers.  The two dispatch functions share
 * no code, so a check present in one and missing from the other is the shape
 * of bug this file exists to catch.
 */

#include <jlib/net/http_server.hh>
#include <jlib/sys/socketstream.hh>

#include <cstdio>
#include <iostream>
#include <sstream>
#include <mutex>
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

/** A tree with something worth stealing outside the root, and inside it. */
struct tree {
    std::string base;
    std::string root;

    tree() {
        char pattern[] = "/tmp/jlib_hostile_XXXXXX";

        base = ::mkdtemp(pattern);
        root = base + "/www";

        ::mkdir(root.c_str(), 0755);
        ::mkdir((root + "/private").c_str(), 0755);

        write(root + "/page.html", "<h1>x</h1>\n");
        write(root + "/private/key.txt", "THE KEY\n");
    }

    ~tree() {
        const std::string rm = "rm -rf '" + base + "'";

        if(std::system(rm.c_str()) != 0) { }
    }

    static void write(const std::string& path, const std::string& text) {
        std::FILE* f = std::fopen(path.c_str(), "w");

        if(f) { std::fputs(text.c_str(), f); std::fclose(f); }
    }
};

/** Everything the server said, raw. */
static std::string raw_exchange(unsigned short port, const std::string& raw) {
    try {
        sys::socketstream s("127.0.0.1", port, 5);

        s.set_timeout(5);
        s.write(raw.data(), std::streamsize(raw.size()));
        s.flush();

        std::ostringstream all;

        all << s.rdbuf();

        return all.str();
    }
    catch(std::exception&) {
        return std::string();
    }
}

static std::string get(unsigned short port, const std::string& target) {
    return raw_exchange(port, "GET " + target +
                    " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
}

static int status_of(const std::string& r) {
    if(r.size() < 12 || r.compare(0, 5, "HTTP/") != 0) return 0;

    return std::atoi(r.c_str() + 9);
}

static std::string body_of(const std::string& r) {
    const std::string::size_type i = r.find("\r\n\r\n");

    return i == std::string::npos ? std::string() : r.substr(i + 4);
}

/**
 * A 400 must not quote the client back to itself.
 *
 * util::http builds genuinely useful messages by naming the offending value.
 * Sending one to the client reflects attacker-chosen bytes into a response
 * body -- measured during the audit, with the whole of a <script> tag coming
 * back verbatim.  The server answers 400 and says nothing else.
 */
static void a_refusal_does_not_quote_the_client(http::server& s) {
    std::cout << "\nwhat a 400 gives back:\n";

    const char* marker = "REFLECT<script>alert(1)</script>ME";

    struct { std::string raw; const char* why; } cases[] = {
        { std::string("POST /ok HTTP/1.1\r\nHost: x\r\nContent-Length: ") +
          marker + "\r\n\r\n", "a Content-Length that is not a number" },
        { std::string("POST /ok HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ") +
          marker + "\r\n\r\n", "a Transfer-Encoding it cannot frame" },
        { std::string("GET /ok HTTP/1.1\r\nHost: x\r\nX-A: one\r\n  ") +
          marker + "\r\n\r\n", "an obs-fold continuation" }
    };

    for(std::size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const std::string r = raw_exchange(s.port(), cases[i].raw);
        const std::string body = body_of(r);

        ok(std::string("  ") + cases[i].why + " is refused",
           status_of(r) == 400, std::to_string(status_of(r)));

        ok(std::string("  ") + cases[i].why + ", without quoting it back",
           body.find("REFLECT") == std::string::npos &&
           body.find("<script>") == std::string::npos,
           body.substr(0, 60));
    }
}

/**
 * A control character in the decoded target.
 *
 * Everything above the filesystem works on std::string, which holds a NUL
 * happily; every filesystem call takes c_str(), which stops at one.  So
 * `/static/page.html%00.jpg` was one name to the router and a shorter one to
 * open() -- it returned 200 and the contents of page.html, for a name that was
 * never opened.
 */
static void a_control_character_in_the_target(http::server& s) {
    std::cout << "\na control character in the target:\n";

    struct { const char* target; const char* why; } cases[] = {
        { "/static/page.html%00.jpg", "a NUL, which c_str() would truncate at" },
        { "/static/page%0a.html",     "a newline" },
        { "/static/page%0d.html",     "a carriage return" },
        { "/static/page%7f.html",     "DEL" }
    };

    for(std::size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const std::string r = get(s.port(), cases[i].target);

        ok(std::string("  ") + cases[i].why + " is refused",
           status_of(r) == 400, std::to_string(status_of(r)));

        ok(std::string("  ") + cases[i].why + ", and serves nothing",
           body_of(r).find("<h1>") == std::string::npos);
    }
}

/**
 * The name asked for must be the name on disk.
 *
 * **This was an authentication bypass**, and it is the audit's find. macOS and
 * Windows have case-insensitive filesystems; `protect()` compares path text
 * case-sensitively, because that is what an HTTP path is. Where they disagree
 * the filesystem wins, because it is the one that opens the file.
 *
 * Measured before the fix: with `protect("/static/private/*")` over
 * `files("/static/*")`, `/static/PRIVATE/key.txt` missed the guard, matched the
 * route, and returned 200 with the guarded file in it, while
 * `/static/private/key.txt` correctly returned 401.
 *
 * **On Linux this passes either way**, because the differently-cased name
 * genuinely does not exist. It is asserted on both because the invariant is
 * the same on both and only the way of breaking it is local.
 */
static void the_name_asked_for_is_the_name_on_disk(http::server& s) {
    std::cout << "\nthe name asked for is the name on disk:\n";

    {
        const std::string r = get(s.port(), "/static/private/key.txt");

        ok("  the guarded file, asked for plainly, is guarded",
           status_of(r) == 401, std::to_string(status_of(r)));
    }

    const char* variants[] = {
        "/static/PRIVATE/key.txt",
        "/static/Private/key.txt",
        "/static/private/KEY.TXT",
        "/static/PAGE.HTML",

        // **A case variant wearing a dot segment.**  The comparison is against
        // the path realpath returns, which has already dropped "." and empty
        // segments -- so without normalising the requested side first, these
        // differ by more than case, the check declines to fire, and the bypass
        // comes back wearing a hat.  Found by breaking the normaliser and
        // watching every assertion above still pass.
        "/static/./PRIVATE/key.txt",
        "/static//PRIVATE/key.txt",
        "/static/./PAGE.HTML"
    };

    for(std::size_t i = 0; i < sizeof variants / sizeof variants[0]; i++) {
        const std::string r = get(s.port(), variants[i]);
        const int st = status_of(r);

        // 401 is a fine answer too: the guard matched, which is the case where
        // the directory was spelled right and only the file was not.
        ok(std::string("  ") + variants[i] + " does not serve a file",
           st == 404 || st == 401, std::to_string(st));

        ok(std::string("  ") + variants[i] + " leaks nothing",
           r.find("THE KEY") == std::string::npos &&
           r.find("<h1>") == std::string::npos);
    }
}

/**
 * And the normalisation that fix needs must not refuse legitimate paths.
 *
 * `realpath` drops empty and "." segments, so comparing its result against the
 * raw request would refuse `/static/./page.html` -- which is a correct request
 * and worked before the audit.
 */
static void ordinary_paths_still_work(http::server& s) {
    std::cout << "\nand what should still work, does:\n";

    struct { const char* target; const char* why; } fine[] = {
        { "/static/page.html",    "the ordinary case" },
        { "/static/./page.html",  "a dot segment" },
        { "/static//page.html",   "a doubled slash" }
    };

    for(std::size_t i = 0; i < sizeof fine / sizeof fine[0]; i++) {
        const std::string r = get(s.port(), fine[i].target);

        ok(std::string("  ") + fine[i].why + " is served",
           status_of(r) == 200 &&
           body_of(r).find("<h1>") != std::string::npos,
           std::to_string(status_of(r)));
    }
}

/** What the server last told its operator.  See heard_by_the_operator(). */
static std::mutex said;
static std::string complaint;

/**
 * How many header fields one message may carry.
 *
 * `max_head` caps the head's *size*, and for a while that was taken to bound
 * the count too.  It does not: several hundred short fields fit inside 8 KB,
 * `fields` looks up linearly, and the server does several lookups per request.
 * Measured at 2.57 ms for 700 fields against 0.17 ms for a plain request --
 * about 15x, for a request costing the sender 6 KB.
 *
 * The exact boundary is asserted rather than a comfortable number either side
 * of it, because an off-by-one in a limit is the kind of thing that survives a
 * test of "100 is fine, 1000 is not".
 */
static void too_many_header_fields(http::server& s) {
    std::cout << "\nhow many header fields is too many:\n";

    // The request line is not a field; Host is.  So `extra` X-headers plus
    // Host is `extra + 1` fields, and the limit is 100.
    struct { int extra; bool served; const char* why; } cases[] = {
        { 20,  true,  "what a browser sends" },
        { 99,  true,  "one under the limit" },
        { 100, false, "one over it" },
        { 400, false, "far over it" }
    };

    for(std::size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        std::string req = "GET /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n";

        // Connection is a field too, so allow for it.
        for(int n = 0; n < cases[i].extra - 1; n++)
            req += "X" + std::to_string(n) + ": y\r\n";

        req += "\r\n";

        const std::string r = raw_exchange(s.port(), req);

        ok(std::string("  ") + cases[i].why,
           cases[i].served ? status_of(r) == 200 : status_of(r) == 400,
           std::to_string(status_of(r)) + " for " +
           std::to_string(cases[i].extra) + " fields");
    }

    {
        // The refusal is cheap, which is the point: a message over the limit
        // must not first be parsed in full to discover that it is over.
        std::string req = "GET /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n";

        for(int n = 0; n < 700; n++) req += "X" + std::to_string(n) + ": y\r\n";

        req += "\r\n";

        raw_exchange(s.port(), req);

        std::string seen;

        { std::lock_guard<std::mutex> hold(said); seen = complaint; }

        ok("  and the operator is told how many there were",
           seen.find("too many header fields") != std::string::npos, seen);
    }
}

/**
 * A refusal the client cannot read must still be one the operator can.
 *
 * Withholding the diagnosis from the client is only free because it goes
 * somewhere else.  Before the audit it went nowhere at all: a smuggling
 * attempt answered 400 and vanished, so the only account of an attack in
 * progress was the one handed to the attacker.
 */
static void heard_by_the_operator(http::server& s) {
    std::cout << "\nand the operator is told what the client is not:\n";

    { std::lock_guard<std::mutex> hold(said); complaint.clear(); }

    const std::string r = raw_exchange(
        s.port(),
        "POST /ok HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: 6\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");

    ok("  the smuggle is refused", status_of(r) == 400,
       std::to_string(status_of(r)));

    ok("  the client is told nothing",
       body_of(r).find("Transfer-Encoding") == std::string::npos,
       body_of(r));

    std::string seen;

    { std::lock_guard<std::mutex> hold(said); seen = complaint; }

    ok("  and the operator is told which rule fired",
       seen.find("depends on who is reading it") != std::string::npos, seen);
}

static void furnish(http::server& s, const tree& t) {
    s.route("GET", "/ok", [](const http::server::Request&,
                             http::server::response& r) {
        r.status(200).type("text/plain").body("ok");
    });

    s.files("/static/*", t.root);

    s.protect("/static/private/*", "Basic realm=\"jlib\"",
              [](const http::server::credentials& c) {
                  return c.user == "root" && c.password == "hunter2";
              });
}

static void everything(http::server& s) {
    a_refusal_does_not_quote_the_client(s);
    a_control_character_in_the_target(s);
    the_name_asked_for_is_the_name_on_disk(s);
    ordinary_paths_still_work(s);
    heard_by_the_operator(s);
    too_many_header_fields(s);
}

int main() {
    std::cout << "net_http_hostile_test\n";

    try {
        tree t;

        {
            http::server s(0, "127.0.0.1");

            furnish(s, t);
            s.transport().on_error([](const std::exception& e, const sys::peer&) {
                std::lock_guard<std::mutex> hold(said);

                complaint = e.what();
            });

            running go(s);

            std::cout << "\n-- the blocking server --";
            everything(s);
        }

        {
            http::server s(http::server::async_t(), 0, "127.0.0.1");

            furnish(s, t);
            s.transport().on_error([](const std::exception& e, const sys::peer&) {
                std::lock_guard<std::mutex> hold(said);

                complaint = e.what();
            });

            running go(s);

            std::cout << "\n-- the async server --";
            everything(s);
        }
    }
    catch(std::exception& e) {
        std::cerr << "net_http_hostile_test: " << e.what() << "\n";
        return 1;
    }

    if(failures) {
        std::cerr << "net_http_hostile_test: " << failures << " failed\n";
        return 1;
    }

    std::cout << "net_http_hostile_test: all good\n";

    return 0;
}
