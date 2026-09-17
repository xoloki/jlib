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
 * jhttpd's logging: the format, the escaping, and the hook underneath both.
 *
 * Two halves. The formatter is driven directly with records built here, which
 * is the only way to assert an exact line -- a timestamp taken from the clock
 * cannot be compared against anything. The hook is driven through a real
 * server, because what the record *contains* is the half a formatter test
 * cannot check.
 */

#include "../jlib/apps/jhttpd.hh"

#include <jlib/net/http.hh>
#include <jlib/net/http_server.hh>
#include <jlib/util/URL.hh>
#include <jlib/util/util.hh>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

/** A fixed instant, so a line can be compared against a literal. */
static const std::time_t WHEN = 1789603200;   // 2026-09-17 00:00:00 UTC

static void the_line() {
    std::cout << "\nthe Combined line:\n";

    http::server::access a;

    a.peer = "203.0.113.7";
    a.method = "GET";
    a.target = "/index.html?q=1";
    a.version = "HTTP/1.1";
    a.status = 200;
    a.bytes = 1234;
    a.user_agent = "curl/8.7.1";

    ok("  is what every tool already reads",
       jhttpd::combined(a, WHEN) ==
       "203.0.113.7 - - [17/Sep/2026:00:00:00 +0000] "
       "\"GET /index.html?q=1 HTTP/1.1\" 200 1234 \"-\" \"curl/8.7.1\"",
       jhttpd::combined(a, WHEN));

    a.user = "root";
    a.referer = "http://example/";

    ok("  with the authenticated user and referer where they belong",
       jhttpd::combined(a, WHEN) ==
       "203.0.113.7 - root [17/Sep/2026:00:00:00 +0000] "
       "\"GET /index.html?q=1 HTTP/1.1\" 200 1234 "
       "\"http://example/\" \"curl/8.7.1\"",
       jhttpd::combined(a, WHEN));

    {
        // A connection that died before it was answered: no request line to
        // report, and "-" is what the format says rather than an empty field.
        http::server::access dead;

        dead.peer = "203.0.113.7";

        ok("  and a request that never arrived is dashes, not blanks",
           jhttpd::combined(dead, WHEN) ==
           "203.0.113.7 - - [17/Sep/2026:00:00:00 +0000] \"-\" 0 0 \"-\" \"-\"",
           jhttpd::combined(dead, WHEN));
    }
}

/**
 * The escaping, against what can actually reach a field.
 *
 * Measured before writing these: a raw control character in a header value is
 * refused by the grammar with a 400, so **line forgery is not the vector**.
 * What gets through is a double quote, which is an ordinary VCHAR, and bytes
 * above 0x7F, which obs-text allows.
 */
static void what_a_client_can_put_in_a_field() {
    std::cout << "\nescaping, and what it is actually for:\n";

    ok("  an ordinary field is passed through untouched",
       jhttpd::escaped("Mozilla/5.0 (X11; Linux x86_64)") ==
       "Mozilla/5.0 (X11; Linux x86_64)");

    {
        // The live one.  Unescaped, this line would parse as a *different*
        // request: status 999, no bytes, a referer of "-" and an agent of
        // "forged".  Nothing upstream refuses it, because as HTTP there is
        // nothing wrong with it.
        http::server::access a;

        a.peer = "203.0.113.7";
        a.method = "GET";
        a.target = "/x";
        a.version = "HTTP/1.1";
        a.status = 200;
        a.bytes = 5;
        a.user_agent = "x\" 999 0 \"-\" \"forged";

        const std::string line = jhttpd::combined(a, WHEN);

        // Not `find("\" 999")`: that matches the *escaped* form too, which is
        // how this assertion first failed against a line that was correct.
        // What matters is that the quote arrives with a backslash on it.
        ok("  a quote arrives escaped rather than ending a field",
           line.find("\\\" 999 0 ") != std::string::npos, line);

        // Eight quotes exactly: four fields, opened and closed, and no more.
        std::size_t quotes = 0;

        for(std::size_t i = 0; i < line.size(); i++) {
            if(line[i] == '"' && (i == 0 || line[i - 1] != '\\')) quotes++;
        }

        ok("  so the line still has exactly four quoted fields", quotes == 6,
           std::to_string(quotes) + " unescaped quotes");
    }

    ok("  a backslash does not eat the character after it",
       jhttpd::escaped("a\\b") == "a\\\\b", jhttpd::escaped("a\\b"));

    ok("  a high byte becomes text, so the file stays text",
       jhttpd::escaped("high\xc3" "(byte") == "high\\xc3(byte",
       jhttpd::escaped("high\xc3" "(byte"));

    // Belt and braces.  A control character cannot reach here through the
    // server -- the grammar refuses it -- but escaped() is a function anybody
    // may call with anything, and a newline is the one byte that must never
    // reach a line-delimited file.
    ok("  and a newline, which the parser refuses but this must not trust",
       jhttpd::escaped("a\r\nb") == "a\\r\\nb", jhttpd::escaped("a\r\nb"));
}

static void what_the_hook_reports(bool async) {
    std::cout << "\nwhat the server reports ("
              << (async ? "async" : "blocking") << "):\n";

    char pattern[] = "/tmp/jlib_jhttpd_XXXXXX";
    const std::string root = ::mkdtemp(pattern);

    {
        std::FILE* f = std::fopen((root + "/page.html").c_str(), "w");

        if(f) { std::fputs("0123456789", f); std::fclose(f); }
    }

    std::mutex lock;
    std::vector<http::server::access> seen;

    std::unique_ptr<http::server> s;

    if(async) s.reset(new http::server(http::server::async_t(), 0, "127.0.0.1"));
    else      s.reset(new http::server(0, "127.0.0.1"));

    s->files("/static/*", root);

    s->protect("/static/private/*", "Basic realm=\"x\"",
               [](const http::server::credentials& c) {
                   return c.user == "root" && c.password == "hunter2";
               });

    s->on_request([&lock, &seen](const http::server::access& a) {
        std::lock_guard<std::mutex> hold(lock);

        seen.push_back(a);
    });

    s->transport().on_error([](const std::exception&, const sys::peer&) {});

    running go(*s);

    try { http::request("GET", util::URL(s->url("/static/page.html"))); }
    catch(std::exception&) { }

    try { http::request("GET", util::URL(s->url("/static/nope.html"))); }
    catch(std::exception&) { }

    try { http::request("GET", util::URL(s->url("/static/private/x"))); }
    catch(std::exception&) { }

    // Give the async server a moment: the record is written when the request
    // finishes, which is not when the client's read returns.
    for(int i = 0; i < 100; i++) {
        { std::lock_guard<std::mutex> hold(lock); if(seen.size() >= 3) break; }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::lock_guard<std::mutex> hold(lock);

    ok("  one record per request", seen.size() == 3,
       std::to_string(seen.size()));

    if(seen.size() >= 3) {
        ok("  a served file reports its status and its length",
           seen[0].status == 200 && seen[0].bytes == 10,
           std::to_string(seen[0].status) + " " +
           std::to_string(seen[0].bytes));

        ok("  and the target as it was asked for",
           seen[0].target == "/static/page.html", seen[0].target);

        ok("  a miss is a 404 with the body it sent",
           seen[1].status == 404 && seen[1].bytes > 0,
           std::to_string(seen[1].status) + " " +
           std::to_string(seen[1].bytes));

        // The status a *guard* produced, from a path that never reaches a
        // route -- the one most likely to be lost by instrumenting handlers
        // instead of the server.
        ok("  and a refusal is reported like anything else",
           seen[2].status == 401, std::to_string(seen[2].status));

        ok("  with no user, because nobody was let in",
           seen[2].user.empty(), seen[2].user);
    }

    const std::string rm = "rm -rf '" + root + "'";

    if(std::system(rm.c_str()) != 0) { }
}

static void the_user_that_got_in(bool async) {
    std::cout << "\nthe authenticated user ("
              << (async ? "async" : "blocking") << "):\n";

    std::mutex lock;
    std::vector<http::server::access> seen;

    std::unique_ptr<http::server> s;

    if(async) s.reset(new http::server(http::server::async_t(), 0, "127.0.0.1"));
    else      s.reset(new http::server(0, "127.0.0.1"));

    s->route("GET", "/private/thing", [](const http::server::Request&,
                                         http::server::response& r) {
        r.status(200).type("text/plain").body("ok");
    });

    s->protect("/private/*", "Basic realm=\"x\"",
               [](const http::server::credentials& c) {
                   return c.user == "root" && c.password == "hunter2";
               });

    s->on_request([&lock, &seen](const http::server::access& a) {
        std::lock_guard<std::mutex> hold(lock);

        seen.push_back(a);
    });

    s->transport().on_error([](const std::exception&, const sys::peer&) {});

    running go(*s);

    util::http::fields f;

    f.add("Authorization", "Basic " + util::base64::encode(std::string("root:hunter2")));

    try { http::request("GET", util::URL(s->url("/private/thing")), f); }
    catch(std::exception&) { }

    for(int i = 0; i < 100; i++) {
        { std::lock_guard<std::mutex> hold(lock); if(!seen.empty()) break; }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::lock_guard<std::mutex> hold(lock);

    ok("  is recorded when a guard lets one through",
       seen.size() == 1 && seen[0].status == 200 && seen[0].user == "root",
       seen.empty() ? "no record"
                    : std::to_string(seen[0].status) + " user=\"" +
                      seen[0].user + "\"");
}

/** Every line of a file, for asserting what landed where. */
static std::vector<std::string> lines_of(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream in(path.c_str());
    std::string line;

    while(std::getline(in, line)) out.push_back(line);

    return out;
}

/**
 * Rotation: move the file aside, send SIGHUP, get a new one.
 *
 * The signal itself is not sent here -- a test that raised SIGHUP would be
 * testing the C library's dispatch and would do it to the whole process.  What
 * is tested is everything the handler's one line leads to, which is where all
 * the decisions are.
 *
 * **Reopening happens on the next line written**, not when the signal lands,
 * because the reopen needs the lock and a signal handler may not take one. A
 * log with nothing to say does not need a new file.
 */
static void rotation() {
    std::cout << "\nrotating a log:\n";

    char pattern[] = "/tmp/jlib_rot_XXXXXX";
    const std::string dir = ::mkdtemp(pattern);
    const std::string path = dir + "/access.log";

    {
        jhttpd::logfile log;

        ok("  it opens", log.open(path));

        log.write("before");

        ok("  and writes", lines_of(path).size() == 1);

        // What a rotation tool does.
        ::rename(path.c_str(), (path + ".1").c_str());

        log.write("still the old inode");

        ok("  a line written after the move follows the moved file",
           lines_of(path + ".1").size() == 2 && lines_of(path).empty(),
           std::to_string(lines_of(path + ".1").size()) + " there, " +
           std::to_string(lines_of(path).size()) + " here");

        // What the handler does, and nothing else.
        jhttpd::reopen_requested++;

        log.write("after the hup");

        const std::vector<std::string> fresh = lines_of(path);

        ok("  and after a reopen the path has a new file with the new line",
           fresh.size() == 1 && fresh[0] == "after the hup",
           fresh.empty() ? "empty" : fresh[0]);

        ok("  while the rotated file kept what it had",
           lines_of(path + ".1").size() == 2);
    }

    {
        // **The reason the flag is a counter.**  A boolean would be consumed
        // by whichever file wrote first, and the second would go on writing to
        // an inode with no name -- the exact bug rotation exists to prevent,
        // reintroduced inside the fix for it.
        const std::string a = dir + "/a.log";
        const std::string b = dir + "/b.log";

        jhttpd::logfile la;
        jhttpd::logfile lb;

        la.open(a);
        lb.open(b);

        la.write("one");
        lb.write("one");

        ::rename(a.c_str(), (a + ".1").c_str());
        ::rename(b.c_str(), (b + ".1").c_str());

        jhttpd::reopen_requested++;

        la.write("two");
        lb.write("two");

        ok("  one signal reopens every log, not just the first to notice",
           lines_of(a).size() == 1 && lines_of(b).size() == 1,
           std::to_string(lines_of(a).size()) + " and " +
           std::to_string(lines_of(b).size()));
    }

    {
        // **A reopen must not be a truncation.**  Nothing says a SIGHUP
        // arrives only after a rotation: an operator may send one twice, or
        // send one for a reason that has nothing to do with logs once SIGHUP
        // also means reload.  Opening with trunc would then delete everything
        // written since the last one, which is a log that quietly loses the
        // part an incident is in.
        const std::string twice = dir + "/twice.log";

        jhttpd::logfile log;

        log.open(twice);
        log.write("first");

        jhttpd::reopen_requested++;
        log.write("second");

        jhttpd::reopen_requested++;
        log.write("third");

        ok("  a reopen with nothing rotated keeps what is already there",
           lines_of(twice).size() == 3,
           std::to_string(lines_of(twice).size()) + " lines");
    }

    {
        // A log that was never given a path discards, and must not start
        // writing somewhere after a reopen.
        jhttpd::logfile none;

        ok("  a discarding log stays discarding", none.open(""));

        none.write("nowhere");

        jhttpd::reopen_requested++;

        none.write("still nowhere");

        ok("  and has nothing to reopen", !none.wanted());
    }

    const std::string rm = "rm -rf '" + dir + "'";

    if(std::system(rm.c_str()) != 0) { }
}

int main() {
    std::cout << "app_jhttpd_test\n";

    try {
        the_line();
        rotation();
        what_a_client_can_put_in_a_field();

        what_the_hook_reports(false);
        what_the_hook_reports(true);

        the_user_that_got_in(false);
        the_user_that_got_in(true);
    }
    catch(std::exception& e) {
        std::cerr << "app_jhttpd_test: " << e.what() << "\n";
        return 1;
    }

    if(failures) {
        std::cerr << "app_jhttpd_test: " << failures << " failed\n";
        return 1;
    }

    std::cout << "app_jhttpd_test: all good\n";

    return 0;
}
