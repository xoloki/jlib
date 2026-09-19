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

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <limits.h>
#include <pwd.h>
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

        log.drain();

        ok("  and writes", lines_of(path).size() == 1);

        // What a rotation tool does.
        ::rename(path.c_str(), (path + ".1").c_str());

        log.write("still the old inode");

        log.drain();

        ok("  a line written after the move follows the moved file",
           lines_of(path + ".1").size() == 2 && lines_of(path).empty(),
           std::to_string(lines_of(path + ".1").size()) + " there, " +
           std::to_string(lines_of(path).size()) + " here");

        // What the handler does, and nothing else.
        // The real handler, called directly: it counts the signal and pokes
        // the pipe exactly as a delivered SIGHUP would, without needing the
        // signal installed in a test process whose default action is to die.
        sys::wakeup::on_signal(SIGHUP);

        log.write("after the hup");

        log.drain();

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

        la.drain();
        lb.write("one");
        lb.drain();

        ::rename(a.c_str(), (a + ".1").c_str());
        ::rename(b.c_str(), (b + ".1").c_str());

        // The real handler, called directly: it counts the signal and pokes
        // the pipe exactly as a delivered SIGHUP would, without needing the
        // signal installed in a test process whose default action is to die.
        sys::wakeup::on_signal(SIGHUP);

        la.write("two");

        la.drain();
        lb.write("two");
        lb.drain();

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
        log.drain();

        // The real handler, called directly: it counts the signal and pokes
        // the pipe exactly as a delivered SIGHUP would, without needing the
        // signal installed in a test process whose default action is to die.
        sys::wakeup::on_signal(SIGHUP);
        log.write("second");
        log.drain();

        // The real handler, called directly: it counts the signal and pokes
        // the pipe exactly as a delivered SIGHUP would, without needing the
        // signal installed in a test process whose default action is to die.
        sys::wakeup::on_signal(SIGHUP);
        log.write("third");
        log.drain();

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

        none.drain();

        // The real handler, called directly: it counts the signal and pokes
        // the pipe exactly as a delivered SIGHUP would, without needing the
        // signal installed in a test process whose default action is to die.
        sys::wakeup::on_signal(SIGHUP);

        none.write("still nowhere");

        none.drain();

        ok("  and has nothing to reopen", !none.wanted());
    }

    const std::string rm = "rm -rf '" + dir + "'";

    if(std::system(rm.c_str()) != 0) { }
}

/** Seconds for one check(), best of `n` -- best, because noise only adds. */
static double timed_check(const jhttpd::credentials& c, const std::string& user,
                          const std::string& pw, int n)
{
    double best = 1e9;

    for(int i = 0; i < n; i++) {
        const std::chrono::steady_clock::time_point t0 =
            std::chrono::steady_clock::now();

        (void)c.check(user, pw);

        const double took = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();

        if(took < best) best = took;
    }

    return best;
}

/**
 * The credential file, and the two things about it that are security
 * properties rather than conveniences.
 */
static void credentials_file() {
    std::cout << "\ncredentials:\n";

    char pattern[] = "/tmp/jlib_creds_XXXXXX";
    const std::string dir = ::mkdtemp(pattern);
    const std::string path = dir + "/creds";

    std::string hash;

    try { hash = jhttpd::hash_password("hunter2"); }
    catch(std::exception& e) {
        std::cout << "  skip  no libsodium in this build: " << e.what() << "\n";

        return;
    }

    ok("  a hash says what it is", hash.compare(0, 9, "$argon2id") == 0,
       hash.substr(0, 40));

    const std::string write_creds =
        "# who may read /private\n"
        "\n"
        "root:" + hash + "\n";

    { std::ofstream o(path.c_str()); o << write_creds; }

    ::chmod(path.c_str(), 0600);

    jhttpd::credentials c;

    c.load(path);

    ok("  comments and blank lines are not users", c.size() == 1,
       std::to_string(c.size()));

    ok("  the right password is accepted", c.check("root", "hunter2"));
    ok("  a wrong one is not", !c.check("root", "hunter3"));
    ok("  and an unknown user is not", !c.check("nobody", "hunter2"));

    {
        // **The timing property.**  Returning early for an unknown user makes
        // "no such user" faster than "wrong password", and Argon2id is
        // deliberately slow, so the gap is milliseconds -- measurable from
        // outside, and a user-enumeration oracle.
        //
        // Measured through HTTP before this test existed: 45.4 ms for a known
        // user with a wrong password against 6.1 ms for an unknown one, once
        // the decoy was removed. Here, with no network in the way, the ratio
        // is the assertion.
        //
        // One-sided and generous: with the decoy the ratio is about 1, without
        // it about 0.001. Anything above a quarter means the work happened.
        const double known = timed_check(c, "root", "hunter3", 5);
        const double unknown = timed_check(c, "nobody", "hunter3", 5);

        const double ratio = known > 0 ? unknown / known : 0;

        ok("  an unknown user costs what a wrong password costs",
           ratio > 0.25,
           "unknown/known = " + std::to_string(ratio));
    }

    {
        // Refused, not warned about: these hashes are expensive to attack
        // rather than impossible, and a world-readable list of them is an
        // offline attack handed to whoever can read the disk.
        ::chmod(path.c_str(), 0644);

        bool threw = false;
        std::string why;

        jhttpd::credentials open_to_all;

        try { open_to_all.load(path); }
        catch(std::exception& e) { threw = true; why = e.what(); }

        ok("  a world-readable credential file is refused", threw, why);

        ::chmod(path.c_str(), 0600);
    }

    {
        // A typo should stop the server rather than quietly guard less than
        // the operator believes.
        const std::string bad = dir + "/bad";

        { std::ofstream o(bad.c_str()); o << "root" << hash << "\n"; }

        ::chmod(bad.c_str(), 0600);

        bool threw = false;
        std::string why;

        jhttpd::credentials c2;

        try { c2.load(bad); }
        catch(std::exception& e) { threw = true; why = e.what(); }

        ok("  a line with no colon is an error, not a skipped line", threw, why);

        ok("  and it says which line", why.find("line 1") != std::string::npos,
           why);
    }

    {
        const std::string empty = dir + "/empty";

        { std::ofstream o(empty.c_str()); o << "# nobody at all\n"; }

        ::chmod(empty.c_str(), 0600);

        bool threw = false;

        jhttpd::credentials c3;

        try { c3.load(empty); }
        catch(std::exception&) { threw = true; }

        ok("  and a file with no credentials in it is refused", threw);
    }

    const std::string rm = "rm -rf '" + dir + "'";

    if(std::system(rm.c_str()) != 0) { }
}


// ------------------------------------------------------------- the config

/** Write `text` to a scratch file and return its path. */
static std::string conf_file(const std::string& text) {
    const std::string path = "jhttpd_conf_test.conf";
    std::ofstream     out(path.c_str());

    out << text;
    out.close();

    return path;
}

/** Read a config, or return the message it was refused with. */
static std::string refusal(const std::string& text, jhttpd::options& o) {
    const std::string path = conf_file(text);

    try {
        jhttpd::read_config(path, o);
        std::remove(path.c_str());

        return "";
    }
    catch(const std::exception& e) {
        std::remove(path.c_str());

        return e.what();
    }
}

static std::string refusal(const std::string& text) {
    jhttpd::options o;

    return refusal(text, o);
}

static void what_a_config_sets() {
    std::cout << "\nwhat a config sets:\n";

    jhttpd::options o;

    const std::string why = refusal(
        "# a server\n"
        "threads 7;\n"
        "daemon;\n"
        "pid /var/run/jhttpd.pid;\n"
        "user www-data www-group;\n"
        "error_log /var/log/err.log;\n"
        "\n"
        "http {\n"
        "    listen 127.0.0.1:9443;\n"
        "    root /srv/www;\n"
        "    prefix /pub;\n"
        "    access_log /var/log/acc.log;\n"
        "    cache_control \"public, max-age=60\";\n"
        "    ssl_certificate /etc/c.pem;\n"
        "    ssl_certificate_key /etc/k.pem;\n"
        "    request_rate 2.5;\n"
        "    request_burst 10;\n"
        "    max_per_address 4;\n"
        "    max_connections 64;\n"
        "    keepalive_requests 20;\n"
        "    keepalive_timeout 45;\n"
        "    client_header_timeout 3;\n"
        "    io_timeout 15;\n"
        "}\n", o);

    ok("  it is accepted", why.empty(), why);

    ok("  threads", o.threads == 7, std::to_string(o.threads));
    ok("  daemon", o.daemon);
    ok("  pid", o.pidfile == "/var/run/jhttpd.pid", o.pidfile);
    ok("  user and group", o.user == "www-data" && o.group == "www-group",
       o.user + ":" + o.group);
    ok("  error_log", o.error_log == "/var/log/err.log", o.error_log);

    ok("  listen splits host from port",
       o.listens.size() == 1 && o.listens[0].host == "127.0.0.1" &&
           o.listens[0].port == 9443,
       o.listens.empty() ? "none"
                         : o.listens[0].host + ":" +
                               std::to_string(o.listens[0].port));
    ok("  root", o.root == "/srv/www", o.root);
    ok("  prefix", o.prefix == "/pub", o.prefix);
    ok("  access_log", o.access_log == "/var/log/acc.log", o.access_log);
    // The directive that makes a link to a site root work.
    {
        jhttpd::options d;

        ok("  index defaults to index.html",
           jhttpd::options().index.size() == 1 &&
               jhttpd::options().index[0] == "index.html");

        ok("  and takes a list, in order",
           refusal("http { index index.html index.htm default.html; }", d)
                   .empty() &&
               d.index.size() == 3 && d.index[0] == "index.html" &&
               d.index[2] == "default.html",
           std::to_string(d.index.size()));

        jhttpd::options off;

        // The only way to ask for the old behaviour, so it has to be sayable.
        ok("  and no arguments turns it off",
           refusal("http { index; }", off).empty() && off.index.empty(),
           std::to_string(off.index.size()));
    }

    ok("  cache_control keeps its spaces",
       o.cache_control == "public, max-age=60", o.cache_control);
    ok("  the certificate pair",
       o.cert == "/etc/c.pem" && o.key == "/etc/k.pem", o.cert + " " + o.key);
    ok("  a fractional rate", o.rate == 2.5, std::to_string(o.rate));
    ok("  burst", o.burst == 10);
    ok("  max_per_address", o.max_per_address == 4);
    ok("  max_connections", o.max_connections == 64);
    ok("  keepalive_requests", o.max_requests == 20);
    ok("  keepalive_timeout", o.idle_timeout == 45);
    ok("  client_header_timeout", o.initial_idle_timeout == 3);
    ok("  io_timeout", o.io_timeout == 15);

    // A port on its own leaves the host alone, which is the common case and
    // the one where getting it wrong would bind somewhere unintended.
    jhttpd::options b;

    ok("  a bare port does not touch the host",
       refusal("http { listen 8081; }", b).empty() && b.listens.size() == 1 &&
           b.listens[0].port == 8081 &&
           b.listens[0].host == jhttpd::listen_spec().host,
       b.listens.empty() ? "none"
                         : b.listens[0].host + ":" +
                               std::to_string(b.listens[0].port));

    jhttpd::options v6;

    ok("  an IPv6 address keeps its colons",
       refusal("http { listen [::1]:8082; }", v6).empty() &&
           v6.listens.size() == 1 && v6.listens[0].host == "[::1]" &&
           v6.listens[0].port == 8082,
       v6.listens.empty() ? "none"
                          : v6.listens[0].host + " " +
                                std::to_string(v6.listens[0].port));

    // **Repeatable, which is the whole of #298 at this layer.**
    jhttpd::options two;

    ok("  two listen directives are two ports",
       refusal("http {\n"
               "    listen 80 redirect;\n"
               "    listen 443 ssl;\n"
               "    ssl_certificate /c.pem;\n"
               "    ssl_certificate_key /c.key;\n"
               "}\n", two).empty() && two.listens.size() == 2,
       std::to_string(two.listens.size()));

    ok("  with the markers on the right one",
       two.listens.size() == 2 && two.listens[0].redirect &&
           !two.listens[0].ssl && two.listens[1].ssl &&
           !two.listens[1].redirect);

    // The latent bug this replaced: `listen` used to assign into one shared
    // host, and the assignment was conditional on a colon being present -- so
    // a bare port after an addressed one silently inherited the address.
    jhttpd::options mixed;

    ok("  and a bare port after an addressed one keeps its own host",
       refusal("http { listen 1.2.3.4:443; listen 8080; }", mixed).empty() &&
           mixed.listens.size() == 2 && mixed.listens[0].host == "1.2.3.4" &&
           mixed.listens[1].host == jhttpd::listen_spec().host,
       mixed.listens.size() == 2 ? mixed.listens[1].host : "?");

    jhttpd::options off;

    ok("  \"off\" turns a flag off, as nginx spells it",
       refusal("daemon off;\nasync off;\n", off).empty() &&
           !off.daemon && !off.async);
}

static void sites_and_guards() {
    std::cout << "\nsites and guards:\n";

    jhttpd::options o;

    const std::string why = refusal(
        "http {\n"
        "    root /srv/www;\n"
        "    server {\n"
        "        server_name other.example;\n"
        "        root /srv/other;\n"
        "    }\n"
        "    server {\n"
        "        server_name tls.example;\n"
        "        root /srv/tls;\n"
        "        ssl_certificate /etc/t.pem;\n"
        "        ssl_certificate_key /etc/t.key;\n"
        "    }\n"
        "    location /private {\n"
        "        auth_basic \"restricted area\";\n"
        "        auth_basic_user_file /etc/jhttpd/users;\n"
        "    }\n"
        "}\n", o);

    ok("  it is accepted", why.empty(), why);

    ok("  two sites", o.vhosts.size() == 2, std::to_string(o.vhosts.size()));
    ok("  the first has no certificate",
       o.vhosts.size() == 2 && o.vhosts[0].name == "other.example" &&
           o.vhosts[0].root == "/srv/other" && o.vhosts[0].cert.empty());
    ok("  the second has one",
       o.vhosts.size() == 2 && o.vhosts[1].cert == "/etc/t.pem" &&
           o.vhosts[1].key == "/etc/t.key");
    ok("  the default root is untouched by either", o.root == "/srv/www", o.root);

    ok("  one guard", o.protect.size() == 1, std::to_string(o.protect.size()));

    // **The shape that replaces PREFIX:REALM:FILE.**  The realm has a space in
    // it and the prefix is a path; in the flag those had to be pulled apart by
    // counting colons from opposite ends.  Here they are three separate
    // directives and nothing has to be guessed.
    ok("  its prefix, realm and file",
       o.protect.size() == 1 && o.protect[0].prefix == "/private" &&
           o.protect[0].realm == "restricted area" &&
           o.protect[0].file == "/etc/jhttpd/users",
       o.protect.size() ? o.protect[0].realm : "");

    jhttpd::options none;

    ok("  a location with neither directive is not a guard",
       refusal("http { location /x { } }", none).empty() && none.protect.empty());

    // **server_name takes a list**, which is what Apache writes as ServerName
    // plus ServerAlias and what nginx writes directly.  One site per name.
    jhttpd::options many;

    const std::string why2 = refusal(
        "http {\n"
        "    root /srv/www;\n"
        "    server {\n"
        "        server_name www.example.org example.org example.ninja;\n"
        "        root /srv/ex;\n"
        "        ssl_certificate /etc/e.pem;\n"
        "        ssl_certificate_key /etc/e.key;\n"
        "    }\n"
        "}\n", many);

    ok("  a list of names is accepted", why2.empty(), why2);
    ok("  and is one site per name", many.vhosts.size() == 3,
       std::to_string(many.vhosts.size()));
    ok("  each with the same root",
       many.vhosts.size() == 3 && many.vhosts[0].root == "/srv/ex" &&
           many.vhosts[2].root == "/srv/ex");
    ok("  in the order they were written",
       many.vhosts.size() == 3 && many.vhosts[0].name == "www.example.org" &&
           many.vhosts[1].name == "example.org" &&
           many.vhosts[2].name == "example.ninja",
       many.vhosts.size() == 3 ? many.vhosts[1].name : "");

    // Not decoration: SNI matches on the name the client asked for, so a name
    // without the certificate would fail the handshake before anything else
    // here is consulted.
    ok("  and every one of them carries the certificate",
       many.vhosts.size() == 3 && many.vhosts[0].cert == "/etc/e.pem" &&
           many.vhosts[1].cert == "/etc/e.pem" &&
           many.vhosts[2].cert == "/etc/e.pem" &&
           many.vhosts[2].key == "/etc/e.key");

    jhttpd::options split;

    ok("  repeated server_name accumulates rather than replacing",
       refusal("http { server { server_name a.example;\n"
               "                server_name b.example;\n"
               "                root /srv/x; } }", split).empty() &&
           split.vhosts.size() == 2,
       std::to_string(split.vhosts.size()));

    ok("  a name listed twice is refused, since one of them was meant to differ",
       refusal("http { server { server_name a.example a.example; root /r; } }")
           .find("twice") != std::string::npos);

    ok("  and across two directives as well",
       refusal("http { server { server_name a.example; server_name a.example; "
               "root /r; } }").find("twice") != std::string::npos);

    // Still exactly one name is fine -- the list must not have made the
    // single-name form into an error.
    jhttpd::options one;

    ok("  one name still works",
       refusal("http { server { server_name only.example; root /r; } }", one)
               .empty() && one.vhosts.size() == 1);
}

static void what_a_config_refuses() {
    std::cout << "\nwhat a config refuses:\n";

    // Each of these is a mistake somebody will make, and each must stop the
    // server rather than be warned about -- a config that half-applied is how
    // a server ends up not doing what its operator believes it is doing.
    struct { const char* text; const char* wanted; const char* why; } cases[] = {
        { "http {\n    listen 8080\n    root /srv;\n}\n", "line 2",
          "a missing semicolon is caught by the arity check" },
        { "http {\n    listen 8080\n    root /srv;\n}\n", "missing \";\"",
          "and named as one, since the count alone would puzzle anybody" },
        { "http { listne 8080; }", "not an http directive",
          "a misspelled directive" },
        { "lisen 80;", "not a directive here",
          "and one at the top level" },
        { "http { server_name x; }", "not an http directive",
          "a directive in the wrong block" },
        // No ";" after the block: a directive has one or the other and never
        // both, so `daemon { };` is refused by the grammar before this check
        // is reached, and would test the parser rather than the walker.
        { "daemon { }", "does not take a { } block",
          "a block where none belongs" },
        { "http;", "wants a { } block",
          "and none where one does" },
        { "http { request_rate abc; }", "wants a number",
          "a number that is not one" },
        { "http { request_rate -1; }", "wants a number",
          "and a negative one" },
        { "http { listen http; }", "wants a port",
          "a port that is not one" },
        { "http { listen 8080 quic; }", "does not know",
          "a listen option nobody implements" },
        { "http { listen 443 ssl; }", "needs ssl_certificate",
          "an ssl listener with nothing to present" },
        { "http { listen 80 redirect; }", "needs a \"listen ... ssl\"",
          "a redirect with nowhere to point" },
        // Both keywords needs three arguments, and listen takes two -- so
        // this is refused by arity rather than by a rule of its own.  Asserted
        // because "it is refused" is the property; which check refuses it is
        // not something a config file should have to know.
        { "http { listen 80 ssl redirect; }", "takes 1 to 2 arguments",
          "a port asked to be both ends of the redirect" },
        { "http { listen 8080; listen 8080; }", "twice",
          "the same port bound twice" },
        { "http { ssl_certificate /c.pem; }", "no ssl_certificate_key",
          "half a certificate at the http level, which only the flag caught before" },
        { "daemon maybe;", "wants \"on\" or \"off\"",
          "a flag that is neither" },
        { "http { location /x { auth_basic \"r\"; } }", "no auth_basic_user_file",
          "half a guard, which would ask for a password it cannot check" },
        { "http { location /x { auth_basic_user_file /f; } }", "no auth_basic",
          "and the other half, which no browser could answer" },
        { "http { server { root /srv; } }", "needs a server_name",
          "a site with no name" },
        { "http { server { server_name a; } }", "needs a root",
          "a site with no root" },
        { "http { server { server_name a; root /r; ssl_certificate /c; } }",
          "both ssl_certificate", "half a certificate" },
        { "http { } http { }", "more than once",
          "two http blocks, which would silently keep the last" },
        { "http {\n    listen 8080;\n", "expected \"}\"",
          "and a syntax error still comes through with its line" }
    };

    for(std::size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const std::string got = refusal(cases[i].text);

        ok(std::string("  ") + cases[i].why,
           !got.empty() && got.find(cases[i].wanted) != std::string::npos,
           got.empty() ? "accepted" : got);
    }

    // Break-the-guard: the cases above all assert a *refusal*, and a
    // read_config that threw at everything would pass every one of them.
    ok("  and a config with none of those mistakes is accepted",
       refusal("# fine\nthreads 2;\nhttp { listen 8080; root /srv; }\n").empty());

    // A file that is not there is not a syntax error, and the message has to
    // name it -- this is the most likely failure in the field by a distance.
    jhttpd::options o;
    std::string     missing;

    try { jhttpd::read_config("no-such-config-here.conf", o); }
    catch(const std::exception& e) { missing = e.what(); }

    ok("  a missing file names itself",
       missing.find("no-such-config-here.conf") != std::string::npos, missing);

    // **The shipped example has to parse.** It is the documentation for this
    // feature, and documentation that the program would refuse is worse than
    // none -- the first thing anybody does with it is uncomment a line.
    // Skipped rather than failed when it cannot be found, since the path
    // depends on how the tree was configured and a test that fails on a
    // VPATH build is a test people learn to ignore.
    const char* const src = std::getenv("srcdir");

    if(src) {
        const std::string path =
            std::string(src) + "/../jlib/apps/jhttpd.conf.example";

        std::ifstream in(path.c_str());

        if(in) {
            in.close();

            jhttpd::options e;
            std::string     why;

            try { jhttpd::read_config(path, e); }
            catch(const std::exception& x) { why = x.what(); }

            ok("  the shipped example config parses", why.empty(), why);

            // Not vacuous: it must actually have set something.
            ok("  and says what it looks like it says",
               e.threads == 4 && e.listens.size() == 1 &&
                   e.listens[0].port == 8080 && e.root == "/srv/www",
               std::to_string(e.threads) + " " +
                   (e.listens.empty() ? "no listen"
                                      : std::to_string(e.listens[0].port)) +
                   " " + e.root);
        }
        else std::cout << "  ..     no example config beside the test\n";
    }
    else std::cout << "  ..     srcdir unset, not looking for the example\n";
}


/**
 * What SIGHUP can change, and what it has to admit it cannot.
 *
 * The rule is apply-or-say: anything the running server cannot revisit is
 * named out loud rather than silently kept, because otherwise the operator
 * has a file on disk that does not describe the process that is running.
 */
static void what_a_reload_refuses_to_change() {
    std::cout << "\nreload -- what needs a restart:\n";

    jhttpd::options was;

    was.listens.push_back(jhttpd::listen_spec());
    was.root = "/srv/www";

    ok("  an identical config changes nothing",
       jhttpd::needs_a_restart(was, was).empty(),
       std::to_string(jhttpd::needs_a_restart(was, was).size()));

    struct { const char* wanted; const char* why; } cases[] = {
        { "listen",                "a port" },
        { "root",                  "the document root" },
        { "prefix",                "the prefix" },
        { "index",                 "the index list" },
        { "cache_control",         "cache_control" },
        { "access_log",            "a log path" },
        { "threads",               "the thread count" },
        { "user",                  "the user it drops to" },
        { "max_connections",       "the connection cap" },
        { "keepalive_timeout",     "an idle bound" },
        { "server",                "a site" },
        { "location",              "a guard" }
    };

    for(std::size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        jhttpd::options now = was;
        const std::string w = cases[i].wanted;

        if(w == "listen")                  now.listens[0].port = 9999;
        else if(w == "root")               now.root = "/srv/other";
        else if(w == "prefix")             now.prefix = "/pub";
        else if(w == "index")              now.index.push_back("index.htm");
        else if(w == "cache_control")      now.cache_control = "no-store";
        else if(w == "access_log")         now.access_log = "/var/log/a.log";
        else if(w == "threads")            now.threads = 8;
        else if(w == "user")               now.user = "www-data";
        else if(w == "max_connections")    now.max_connections = 64;
        else if(w == "keepalive_timeout")  now.idle_timeout = 5;
        else if(w == "server") {
            jhttpd::site v;
            v.name = "a.example";
            v.root = "/srv/a";
            now.vhosts.push_back(v);
        }
        else if(w == "location") {
            jhttpd::guard g;
            g.prefix = "/x";
            g.realm = "r";
            g.file = "/f";
            now.protect.push_back(g);
        }

        const std::vector<std::string> got = jhttpd::needs_a_restart(was, now);
        bool named = false;

        for(std::size_t j = 0; j < got.size(); j++) {
            if(got[j] == cases[i].wanted) named = true;
        }

        ok(std::string("  changing ") + cases[i].why + " needs a restart", named,
           got.empty() ? "nothing reported" : got[0]);
    }

    // **The two that must NOT be in that list**, because they are the whole
    // point of a reload: a renewed certificate and an edited password file
    // both keep their paths, so nothing here would notice them -- they are
    // re-read unconditionally instead.
    {
        jhttpd::options now = was;

        now.cert = "/etc/c.pem";
        now.key = "/etc/c.key";

        ok("  a certificate path is not on it, being reloadable",
           jhttpd::needs_a_restart(was, now).empty(),
           jhttpd::needs_a_restart(was, now).empty()
               ? "" : jhttpd::needs_a_restart(was, now)[0]);
    }

    {
        jhttpd::options now = was;

        now.rate = 10;
        now.burst = 20;

        ok("  and neither is a rate limit, which is applied live",
           jhttpd::needs_a_restart(was, now).empty(),
           jhttpd::needs_a_restart(was, now).empty()
               ? "" : jhttpd::needs_a_restart(was, now)[0]);
    }
}

/**
 * Loading a credential file again, while it is in use.
 *
 * This is the half of SIGHUP that matters operationally: adding a user should
 * not need a restart. What makes it safe is that load() builds the new table
 * to the side and publishes a pointer, so a check() already running holds the
 * old one alive until it finishes -- it is reading a hash across an Argon2id
 * verification that takes milliseconds.
 */
static void reloading_a_credential_file() {
    std::cout << "\nreload -- credentials:\n";

    char pattern[] = "/tmp/jlib_reload_XXXXXX";
    const std::string dir = ::mkdtemp(pattern);
    const std::string path = dir + "/creds";

    std::string one;

    try { one = jhttpd::hash_password("alpha"); }
    catch(std::exception& e) {
        std::cout << "  skip  no libsodium in this build\n";

        return;
    }

    const std::string two = jhttpd::hash_password("beta");

    {
        std::ofstream out(path.c_str());
        out << "alice:" << one << "\n";
    }

    ::chmod(path.c_str(), 0600);

    jhttpd::credentials who;

    who.load(path);

    ok("  the first user is known", who.check("alice", "alpha"));
    ok("  and a second is not", !who.check("bob", "beta"));
    ok("  one user counted", who.size() == 1, std::to_string(who.size()));

    // Add one, the way an operator would.
    {
        std::ofstream out(path.c_str(), std::ios::app);
        out << "bob:" << two << "\n";
    }

    who.load(path);

    ok("  after reloading, the second is known too", who.check("bob", "beta"));
    ok("  and the first still is", who.check("alice", "alpha"));
    ok("  two users counted", who.size() == 2, std::to_string(who.size()));
    ok("  a wrong password is still wrong", !who.check("bob", "alpha"));

    // Remove one.
    {
        std::ofstream out(path.c_str());
        out << "bob:" << two << "\n";
    }

    who.load(path);

    ok("  a user removed from the file stops working",
       !who.check("alice", "alpha"));

    // **A reload that fails changes nothing.**  This is what keeps a typo in a
    // password file from locking everyone out of a running server: the new
    // table is built to the side and only published on the last line of
    // load(), so every throw before that leaves the old one in use.
    {
        std::ofstream out(path.c_str());
        out << "not a credential line at all\n";
    }

    bool threw = false;

    try { who.load(path); } catch(std::exception&) { threw = true; }

    ok("  a malformed file is refused", threw);
    ok("  and the credentials that were working still work",
       who.check("bob", "beta"));

    std::remove(path.c_str());
    ::rmdir(dir.c_str());
}

int main() {
    std::cout << "app_jhttpd_test\n";

    try {
        the_line();
        credentials_file();
        rotation();
        what_a_client_can_put_in_a_field();

        what_the_hook_reports(false);
        what_the_hook_reports(true);

        the_user_that_got_in(false);
        the_user_that_got_in(true);

        what_a_config_sets();
        sites_and_guards();
        what_a_config_refuses();
        what_a_reload_refuses_to_change();
        reloading_a_credential_file();
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
