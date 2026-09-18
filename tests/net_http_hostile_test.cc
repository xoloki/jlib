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
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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

    std::string other;

    tree() {
        char pattern[] = "/tmp/jlib_hostile_XXXXXX";

        base = ::mkdtemp(pattern);
        root = base + "/www";
        other = base + "/other";

        ::mkdir(root.c_str(), 0755);
        ::mkdir((root + "/private").c_str(), 0755);
        ::mkdir(other.c_str(), 0755);

        write(root + "/page.html", "<h1>x</h1>\n");
        write(root + "/private/key.txt", "THE KEY\n");

        // A second site, with a file of the same name and different contents,
        // so "which root answered" is visible rather than inferred.
        write(other + "/page.html", "OTHER SITE\n");
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

/**
 * Everything the server said, raw.
 *
 * **A request handed to this must end the connection, or it costs the whole
 * timeout.** The read below runs to end of file, which is the only way to get
 * "everything" from a response whose framing is the thing under test -- a
 * malformed request has no trustworthy Content-Length to read to. So a request
 * that gets a well-formed keep-alive 200 leaves the client sitting in read()
 * until set_timeout() fires, and the case still *passes*, thirty seconds later.
 *
 * That is how it went unnoticed: five such cases in "which site a request
 * names" were costing 150 seconds of a 152-second test, all of it idle, with
 * every assertion green. Measured after: two seconds.
 *
 * Most cases here are refusals, and a 400 closes (http_server.cc says every
 * one does, and the framing ones have to), so they return at once and hide the
 * problem. The ones to watch are the cases that expect 200.
 */
static std::string raw_exchange(unsigned short port, const std::string& raw) {
    try {
        sys::socketstream s("127.0.0.1", port, 5);

        // Generous on purpose: this is the *test's* patience, not the
        // server's deadline, and a short one turns a loaded machine into a
        // false failure. What is being measured is what the server said, never
        // how quickly.
        s.set_timeout(30);
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

/** The site the server decided the last request named.  See on_request(). */
static std::string last_site;

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

/**
 * A handler that asks whether anybody is still waiting.
 *
 * The case it exists for is a handler that produces one whole answer after a
 * long time: it writes nothing until it finishes, so the failed write that
 * tells a *streaming* handler its client left never happens, and it spends the
 * whole time on a reply nobody will read. jserve's non-streaming path holds a
 * model for the length of a generation on exactly that basis.
 *
 * Driven with a raw socket because the point is to hang up **mid-request**,
 * after the head has been read and before the answer -- which a well-behaved
 * client will not do.
 */
static void a_handler_can_ask_if_the_client_left(http::server& s,
                                                 std::atomic<int>& saw)
{
    saw.store(0);

    std::cout << "\nasking whether the client is still there:\n";

    {
        // Still connected: the handler must not be told to give up on
        // somebody who is waiting.  This is the half that catches a check
        // which simply answers "gone".
        const std::string r = get(s.port(), "/slow");

        ok("  a client that waits is not reported gone",
           status_of(r) == 200 && body_of(r).find("finished") != std::string::npos,
           std::to_string(status_of(r)));

        ok("  and the handler saw it as present", saw.load() == 0,
           std::to_string(saw.load()));
    }

    {
        saw.store(0);

        // Send a complete request, then close without reading the answer.
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

        struct sockaddr_in to;

        std::memset(&to, 0, sizeof to);

        to.sin_family = AF_INET;
        to.sin_port = htons(s.port());

        ::inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);

        if(::connect(fd, reinterpret_cast<struct sockaddr*>(&to), sizeof to) == 0) {
            static const char* const req =
                "GET /slow HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";

            if(::write(fd, req, std::strlen(req)) < 0) { }

            // Gone before the handler finishes.  The handler sleeps in small
            // steps and asks between them, so it has a chance to notice.
            std::this_thread::sleep_for(std::chrono::milliseconds(60));

            ::close(fd);
        }

        // One-sided: wait up to two seconds for the handler to notice, and
        // assert only that it did.  A slow machine cannot fail this; only a
        // check that never reports gone can.
        bool noticed = false;

        for(int i = 0; i < 200 && !noticed; i++) {
            if(saw.load() > 0) noticed = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ok("  and a client that hangs up mid-request is", noticed,
           std::to_string(saw.load()));
    }
}

static std::atomic<int> gone_seen{0};

/**
 * `Host`, which nothing read until something needed to route on it.
 *
 * The server matched a method and a path and never looked at the field, so
 * three ways of being ambiguous about it went unnoticed. Two are RFC 9112 3.2
 * MUSTs that were measured answering 200 before this: no Host on HTTP/1.1, and
 * more than one Host. The second is the one that matters -- two Host lines is
 * a routing ambiguity of the shape request smuggling exploits, where a front
 * end believes one and a back end the other.
 *
 * The third is 3.2.2: an **absolute-form** target's authority wins and Host is
 * ignored. That one is not a refusal, so it is asserted through the access
 * record -- which is the only place the decision is visible until something
 * routes on it.
 */
static void what_site_a_request_names(http::server& s) {
    std::cout << "\nwhich site a request names:\n";

    struct { const char* raw; int want; const char* why; } cases[] = {
        { "GET /ok HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n", 200,
          "one Host is what a request should have" },
        { "GET /ok HTTP/1.1\r\n\r\n", 400,
          "none on HTTP/1.1 is a 400, per 3.2" },
        { "GET /ok HTTP/1.0\r\n\r\n", 200,
          "and none on HTTP/1.0 is not, because 1.0 had no Host" },
        { "GET /ok HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400,
          "two is a 400, which is the smuggling shape" },
        { "GET /ok HTTP/1.1\r\nHost: a.example:8080\r\nConnection: close\r\n\r\n", 200,
          "a port is allowed" }
    };

    for(std::size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const std::string r = raw_exchange(s.port(), cases[i].raw);

        ok(std::string("  ") + cases[i].why,
           status_of(r) == cases[i].want,
           std::to_string(status_of(r)));
    }

    // And what the server decided, which a refusal cannot show.
    struct { const char* raw; const char* want; const char* why; } named[] = {
        { "GET /ok HTTP/1.1\r\nHost: A.Example\r\nConnection: close\r\n\r\n", "a.example",
          "Host is lowercased, because DNS is case-insensitive" },
        { "GET /ok HTTP/1.1\r\nHost: a.example:8080\r\nConnection: close\r\n\r\n", "a.example",
          "and its port is not part of the name" },

        // **3.2.2.**  Not a refusal: the RFC says use the target's authority
        // and ignore Host, not that a disagreement is an error.  So the only
        // way to see it is to ask what the server concluded.
        { "GET http://from.target/ok HTTP/1.1\r\nHost: from.field\r\n"
          "Connection: close\r\n\r\n",
          "from.target",
          "an absolute-form target beats the Host field" }
    };

    for(std::size_t i = 0; i < sizeof named / sizeof named[0]; i++) {
        { std::lock_guard<std::mutex> hold(said); last_site.clear(); }

        raw_exchange(s.port(), named[i].raw);

        std::string seen;

        for(int n = 0; n < 100; n++) {
            { std::lock_guard<std::mutex> hold(said); seen = last_site; }

            if(!seen.empty()) break;

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        ok(std::string("  ") + named[i].why, seen == named[i].want,
           "\"" + seen + "\"");
    }
}

/**
 * Virtual hosts: which root answers, and what a client can talk its way into.
 *
 * #270 flagged the shape before this existed: **every `files()` containment
 * property is stated against *a* root, and picking the root from a
 * client-supplied header is a new way to be wrong.** Containment itself is
 * unchanged -- each registration resolves its own root and checks against that
 * one -- so what is left to get wrong is *selection*, and that is what these
 * assert.
 */
/** The first line of a body, so a detail string stays on one line. */
static std::string first_line(const std::string& body) {
    const std::string::size_type nl = body.find('\n');

    return nl == std::string::npos ? body.substr(0, 24)
                                   : body.substr(0, nl);
}

static void which_site_answers(http::server& s, const tree& t) {
    std::cout << "\nwhich site answers:\n";

    struct { const char* host; const char* want; const char* why; } cases[] = {
        { "other.example", "OTHER SITE", "a name with a site of its own" },
        { "OTHER.EXAMPLE", "OTHER SITE", "and the same name shouted" },
        { "other.example:8080", "OTHER SITE", "and with a port on it" },
        { "unclaimed.example", "<h1>x</h1>", "a name nobody claimed gets the default" },
        { "", "<h1>x</h1>", "and so does HTTP/1.0, which has no name to give" }
    };

    for(std::size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        std::string raw;

        if(*cases[i].host == 0) {
            raw = "GET /static/page.html HTTP/1.0\r\nConnection: close\r\n\r\n";
        }
        else {
            raw = std::string("GET /static/page.html HTTP/1.1\r\nHost: ") +
                  cases[i].host + "\r\nConnection: close\r\n\r\n";
        }

        const std::string r = raw_exchange(s.port(), raw);

        ok(std::string("  ") + cases[i].why,
           status_of(r) == 200 &&
           body_of(r).find(cases[i].want) != std::string::npos,
           std::to_string(status_of(r)) + " " + first_line(body_of(r)));
    }

    {
        // RFC 9112 3.2.2, now doing real work: the target's authority selects
        // the site and the field is ignored.  Before virtual hosts this was
        // invisible; it is the host-confusion bug if it goes the other way.
        const std::string r = raw_exchange(
            s.port(),
            "GET http://other.example/static/page.html HTTP/1.1\r\n"
            "Host: unclaimed.example\r\nConnection: close\r\n\r\n");

        ok("  an absolute-form target picks the site, not the Host field",
           body_of(r).find("OTHER SITE") != std::string::npos,
           first_line(body_of(r)));
    }

    {
        // **The containment question.**  A name cannot reach another site's
        // files, because each registration checks against the root it was
        // given -- the Host chooses which registration, never what it may
        // reach.
        const std::string r = raw_exchange(
            s.port(),
            "GET /static/../other/page.html HTTP/1.1\r\n"
            "Host: unclaimed.example\r\nConnection: close\r\n\r\n");

        ok("  and no name reaches another site's root by traversal",
           body_of(r).find("OTHER SITE") == std::string::npos,
           std::to_string(status_of(r)) + " " + first_line(body_of(r)));
    }

    {
        // A guard on one site does not guard another, and -- the direction
        // that matters -- a site does not escape a server-wide guard by
        // existing.
        const std::string r = raw_exchange(
            s.port(),
            "GET /static/private/key.txt HTTP/1.1\r\n"
            "Host: other.example\r\nConnection: close\r\n\r\n");

        ok("  and a site does not escape a server-wide guard",
           status_of(r) == 401 || r.find("THE KEY") == std::string::npos,
           std::to_string(status_of(r)));
    }
}

static void furnish(http::server& s, const tree& t) {
    s.route("GET", "/ok", [](const http::server::Request&,
                             http::server::response& r) {
        r.status(200).type("text/plain").body("ok");
    });

    // A handler that takes its time and writes nothing until the end -- the
    // shape peer_gone() is for.  It asks between steps rather than once, since
    // the answer can change while it works.
    s.route("GET", "/slow",
            [](const http::server::Request&,
               http::server::async_responder& out) -> sys::task<void> {
        // **Twelve, not forty.**  Each step costs two reactor hops as well as
        // its sleep, and the client waiting on the other side has a socket
        // timeout. On a machine busy with something else -- a GPU benchmark
        // next door, in the run that caught this -- eighty hops can outlast
        // that timeout, and the client gives up. The assertion then fails
        // because the request never completed, which says nothing about
        // peer_gone() and reads exactly like a real defect.
        //
        // Twelve is still several chances to notice a departure, and a
        // quarter of the exposure.
        for(int i = 0; i < 12; i++) {
            co_await sys::on_pool(out.pool());

            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            co_await sys::on_reactor(out.reactor());

            if(out.peer_gone()) { gone_seen++; break; }
        }

        http::server::response r;

        r.status(200).type("text/plain").body("finished\n");

        co_await out.send(r);
    });

    s.files("/static/*", t.root);

    // A second site over the same prefix, with its own root.
    s.site_of("other.example").files("/static/*", t.other);

    s.protect("/static/private/*", "Basic realm=\"jlib\"",
              [](const http::server::credentials& c) {
                  return c.user == "root" && c.password == "hunter2";
              });
}

static void everything(http::server& s, const tree& t) {
    a_refusal_does_not_quote_the_client(s);
    a_control_character_in_the_target(s);
    the_name_asked_for_is_the_name_on_disk(s);
    ordinary_paths_still_work(s);
    heard_by_the_operator(s);
    too_many_header_fields(s);
    what_site_a_request_names(s);
    which_site_answers(s, t);
}

int main() {
    // Unbuffered, as 62 of the tests here already are.  Piped anywhere -- which
    // is what `make check` does -- this is otherwise block-buffered and the
    // whole run arrives at exit, so a test that stalls looks like a test that
    // printed nothing and there is no way to see which section it is in.  That
    // cost two measurement attempts while finding the stall fixed above.
    std::cout << std::unitbuf;

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

            s.on_request([](const http::server::access& a) {
                std::lock_guard<std::mutex> hold(said);

                last_site = a.host;
            });

            running go(s);

            std::cout << "\n-- the blocking server --";
            everything(s, t);
        }

        {
            http::server s(http::server::async_t(), 0, "127.0.0.1");

            furnish(s, t);
            s.transport().on_error([](const std::exception& e, const sys::peer&) {
                std::lock_guard<std::mutex> hold(said);

                complaint = e.what();
            });

            s.on_request([](const http::server::access& a) {
                std::lock_guard<std::mutex> hold(said);

                last_site = a.host;
            });

            running go(s);

            std::cout << "\n-- the async server --";
            everything(s, t);

            // **Async only, by construction.**  peer_gone() is on
            // async_responder, so the route that uses it is an async
            // streaming route -- which a blocking server refuses where it
            // reaches it, with a 500.  The blocking half's equivalent is
            // responder::live(), and it learns the same thing the same way a
            // streaming handler always has: from a write that fails.
            a_handler_can_ask_if_the_client_left(s, gone_seen);
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
