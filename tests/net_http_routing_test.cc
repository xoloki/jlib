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
 * net_http_routing_test -- patterns, captures, specificity and 405.
 *
 * Routing is decided before a handler ever runs, so almost all of it can be
 * tested by asking a server which handler answered and what it was told.  The
 * sections below run a real server rather than calling route_for directly,
 * because what a caller can observe is the response -- and because the two
 * servers dispatch separately and both have to agree.
 *
 * The part worth reading twice is specificity: `/static/*` and
 * `/static/index.html` both match `/static/index.html`, and which one wins is
 * a decision rather than an accident.
 */

#include <jlib/net/http.hh>
#include <jlib/net/http_server.hh>
#include <jlib/sys/socketstream.hh>
#include <jlib/util/URL.hh>

#include <iostream>
#include <string>
#include <thread>

using namespace jlib;
using namespace jlib::net;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Every route answers with a line naming itself and what it captured. */
static void furnish(http::server& s) {
    s.route("GET", "/exact", [](const http::server::Request&,
                                http::server::response& r) {
        r.status(200).type("text/plain").body("exact");
    });

    s.route("GET", "/users/{id}", [](const http::server::Request&,
                                     const http::server::params& p,
                                     http::server::response& r) {
        r.status(200).type("text/plain").body("user id=" + p.get("id"));
    });

    s.route("GET", "/users/{id}/posts/{post}",
            [](const http::server::Request&, const http::server::params& p,
               http::server::response& r) {
                r.status(200).type("text/plain")
                 .body("post id=" + p.get("id") + " post=" + p.get("post"));
            });

    // Deliberately registered *before* the literal below, so that a router
    // preferring registration order would get the specificity test wrong.
    s.route("GET", "/static/*", [](const http::server::Request&,
                                   const http::server::params& p,
                                   http::server::response& r) {
        r.status(200).type("text/plain").body("static rest=" + p.rest());
    });

    s.route("GET", "/static/index.html", [](const http::server::Request&,
                                            http::server::response& r) {
        r.status(200).type("text/plain").body("the index itself");
    });

    s.route("GET", "/tagged", [](const http::server::Request&,
                                 http::server::response& r) {
        r.status(200).type("text/plain").field("ETag", "\"v1\"")
         .body("tagged body\n");
    });

    s.route("GET", "/dated", [](const http::server::Request&,
                                http::server::response& r) {
        r.status(200).type("text/plain")
         .field("Last-Modified", "Sun, 06 Nov 1994 08:49:37 GMT")
         .body("dated body\n");
    });

    s.route("POST", "/tagged", [](const http::server::Request&,
                                  http::server::response& r) {
        r.status(200).type("text/plain").field("ETag", "\"v1\"")
         .body("posted\n");
    });

    // A handler with a *strong* validator, which files() never has: its ETag
    // comes from mtime and size and so can only be weak.  This is the only way
    // to reach the If-Range paths that require strong comparison.
    s.route("GET", "/strong", [](const http::server::Request&,
                                 http::server::response& r) {
        r.status(200).type("text/plain").field("ETag", "\"strong-v1\"")
         .body("0123456789abcdefghij");
    });

    // Same path, another method -- what makes 405 possible.
    s.route("POST", "/exact", [](const http::server::Request&,
                                 http::server::response& r) {
        r.status(200).type("text/plain").body("posted");
    });

    s.route("DELETE", "/exact", [](const http::server::Request&,
                                   http::server::response& r) {
        r.status(200).type("text/plain").body("deleted");
    });
}

static util::http::Response ask(http::server& s, const std::string& method,
                                const std::string& path)
{
    return http::request(method, util::URL(s.url(path)));
}

static util::http::Response ask_with(http::server& s, const std::string& method,
                                     const std::string& path,
                                     const std::string& name,
                                     const std::string& value)
{
    util::http::fields f;

    f.add(name, value);

    return http::request(method, util::URL(s.url(path)), f);
}

static void ranges_on_an_ordinary_handler(http::server& s);

/** Both servers, the same table, the same questions. */
static void run_against(http::server& s, const std::string& which) {
    std::cout << "\n" << which << ":\n";

    ok("  an exact route still matches",
       ask(s, "GET", "/exact").body() == "exact");

    {
        const util::http::Response r = ask(s, "GET", "/users/42");

        ok("  a parameter is captured", r.body() == "user id=42", r.body());
    }

    {
        const util::http::Response r = ask(s, "GET", "/users/7/posts/hello");

        ok("  and two of them, by name",
           r.body() == "post id=7 post=hello", r.body());
    }

    {
        const util::http::Response r = ask(s, "GET", "/static/js/app.js");

        ok("  a wildcard takes the rest, however deep",
           r.body() == "static rest=js/app.js", r.body());
    }

    {
        const util::http::Response r = ask(s, "GET", "/static");

        ok("  and matches its own prefix with nothing after it",
           r.body() == "static rest=", r.body());
    }

    {
        // **The specificity rule.**  Both /static/* and /static/index.html
        // match, and the wildcard was registered first -- so a router that
        // took the first match would answer with the wrong one.
        const util::http::Response r = ask(s, "GET", "/static/index.html");

        ok("  a literal beats a wildcard that also matches",
           r.body() == "the index itself", r.body());
    }

    {
        const util::http::Response r = ask(s, "GET", "/nowhere");

        ok("  an unknown path is still 404", r.status() == 404,
           std::to_string(r.status()));
    }

    {
        // **A pattern without a wildcard must account for every segment.**
        // Without that, /exact would match /exact/anything and every route in
        // the table would silently be a prefix -- which is the difference
        // between a router and a set of string comparisons that happen to
        // start the same way.
        const util::http::Response r = ask(s, "GET", "/exact/extra");

        ok("  a path longer than the pattern does not match it",
           r.status() == 404, std::to_string(r.status()) + " " + r.body());
    }

    {
        const util::http::Response r = ask(s, "GET", "/users/42/extra");

        ok("  nor does one longer than a parameterised pattern",
           r.status() == 404, std::to_string(r.status()) + " " + r.body());
    }

    {
        // **A capture is decoded, because the path is.**  Asserted because the
        // comment on params claimed the opposite until path_of was read: it
        // runs uri::decode over the target before routing, so a handler gets
        // text rather than escapes.
        const util::http::Response r = ask(s, "GET", "/users/a%20b");

        ok("  a capture arrives decoded", r.body() == "user id=a b", r.body());
    }

    {
        // And cannot contain a separator: %2F is refused before routing, so a
        // parameter is always one segment's worth of text.
        const util::http::Response r = ask(s, "GET", "/users/a%2Fb");

        ok("  an encoded separator is refused before it can be captured",
           r.status() == 400, std::to_string(r.status()) + " " + r.body());
    }

    {
        // The path exists for GET, POST and DELETE, and PUT is none of them.
        const util::http::Response r = ask(s, "PUT", "/exact");

        ok("  a known path with the wrong method is 405, not 404",
           r.status() == 405, std::to_string(r.status()));

        const std::string allow = r.fields().get("Allow");

        ok("  with an Allow naming every method that would have worked",
           allow.find("GET") != std::string::npos &&
           allow.find("POST") != std::string::npos &&
           allow.find("DELETE") != std::string::npos, allow);

        ok("  and the method that was refused is named in the body",
           r.body().find("PUT") != std::string::npos, r.body());
    }

    {
        // **HEAD is a GET that stops at the headers.**  No HEAD route is
        // registered anywhere in this table; the GET one answers it.
        const util::http::Response g = ask(s, "GET", "/exact");
        const util::http::Response h = ask(s, "HEAD", "/exact");

        ok("  HEAD is answered by the GET route", h.status() == 200,
           std::to_string(h.status()));

        ok("  with no body", h.body().empty(), "\"" + h.body() + "\"");

        // The assertion that makes it a preview of the GET rather than a
        // different question: the length is what the body *would* have been.
        ok("  and the Content-Length the GET would have sent",
           h.fields().get("Content-Length") ==
           std::to_string(g.body().size()),
           h.fields().get("Content-Length") + " vs " +
           std::to_string(g.body().size()));
    }

    {
        const util::http::Response r = ask(s, "HEAD", "/nowhere");

        ok("  and an unrouted path is still 404 for HEAD", r.status() == 404,
           std::to_string(r.status()));
    }

    {
        // If-None-Match against the ETag the handler set.
        const util::http::Response r =
            ask_with(s, "GET", "/tagged", "If-None-Match", "\"v1\"");

        ok("  a matching If-None-Match is 304", r.status() == 304,
           std::to_string(r.status()));

        ok("  with no body", r.body().empty(), "\"" + r.body() + "\"");

        // **A 304 must not claim a length.**  On a reused connection a
        // Content-Length with no body is the next response being read as one.
        ok("  and no Content-Length, which would promise a body",
           !r.fields().has("Content-Length"),
           r.fields().get("Content-Length"));
    }

    {
        const util::http::Response r =
            ask_with(s, "GET", "/tagged", "If-None-Match", "W/\"v1\"");

        ok("  a weak tag matches the strong one it was made from",
           r.status() == 304, std::to_string(r.status()));
    }

    {
        const util::http::Response r =
            ask_with(s, "GET", "/tagged", "If-None-Match", "\"other\", \"v1\"");

        ok("  and so does one named among several", r.status() == 304,
           std::to_string(r.status()));
    }

    {
        const util::http::Response r =
            ask_with(s, "GET", "/tagged", "If-None-Match", "\"v2\"");

        ok("  a tag that does not match is the whole response",
           r.status() == 200 && r.body() == "tagged body\n", r.body());
    }

    {
        // A POST is not safe, and 13.1 does not give a 304 a meaning there.
        const util::http::Response r =
            ask_with(s, "POST", "/tagged", "If-None-Match", "\"v1\"");

        ok("  an unsafe method is never turned into a 304",
           r.status() == 200 && r.body() == "posted\n",
           std::to_string(r.status()) + " " + r.body());
    }

    {
        // **All three date formats**, which is what parsing by the grammar in
        // rfc9110.hh bought over counting characters.  The resource's
        // Last-Modified is 06 Nov 1994 08:49:37 GMT; each of these says the
        // client already has something at least that new.
        const char* const same[] = {
            "Sun, 06 Nov 1994 08:49:37 GMT",        // IMF-fixdate
            "Sunday, 06-Nov-94 08:49:37 GMT",       // RFC 850
            "Sun Nov  6 08:49:37 1994"              // asctime
        };

        for(std::size_t i = 0; i < 3; i++) {
            const util::http::Response r =
                ask_with(s, "GET", "/dated", "If-Modified-Since", same[i]);

            ok(std::string("  If-Modified-Since is read as ") +
               (i == 0 ? "IMF-fixdate" : i == 1 ? "RFC 850" : "asctime"),
               r.status() == 304,
               std::string(same[i]) + " -> " + std::to_string(r.status()));
        }
    }

    {
        const util::http::Response r =
            ask_with(s, "GET", "/dated", "If-Modified-Since",
                     "Sat, 05 Nov 1994 08:49:37 GMT");

        ok("  an older If-Modified-Since gets the whole response",
           r.status() == 200 && r.body() == "dated body\n",
           std::to_string(r.status()));
    }

    {
        // Unreadable, so nothing can be shown to hold -- and the safe way to
        // be wrong is to send everything.
        const util::http::Response r =
            ask_with(s, "GET", "/dated", "If-Modified-Since", "nonsense");

        ok("  an unparseable date is ignored, not believed",
           r.status() == 200, std::to_string(r.status()));
    }

    ranges_on_an_ordinary_handler(s);

    {
        // A path that *does* match for this method must not collect an Allow
        // from the other routes it passed on the way.
        const util::http::Response r = ask(s, "POST", "/exact");

        ok("  a method that does match is answered, not refused",
           r.status() == 200 && r.body() == "posted", r.body());

        ok("  and carries no Allow", !r.fields().has("Allow"),
           r.fields().get("Allow"));
    }
}

/**
 * A HEAD sends the head and stops, proved on the connection.
 *
 * **The obvious assertion cannot see this.**  util::http::parse_head takes a
 * `head_request` flag, so jlib's own client frames a HEAD response as bodyless
 * whatever the server actually sent -- ask it for r.body() and it is empty
 * either way.  Breaking the server's suppression and watching that assertion
 * stay green is how this section came to exist.
 *
 * What a stray body does is corrupt the *next* thing on the connection, so
 * that is what to ask: a HEAD, then a GET, on one socket.  If anything
 * followed the head, the GET's status line is read out of the middle of it.
 */
/**
 * Range on an ordinary handler, and the If-Range comparison that needs a
 * strong validator.
 *
 * **These assertions did not exist until the guards could not be broken.**
 * Range began as a thing only `files()` did, and `files()` sends a weak ETag on
 * purpose -- so the two halves of If-Range's strong comparison (refuse a weak
 * tag from the client, refuse a weak one of our own) were redundant with each
 * other and neither could be shown to matter.  A handler with a strong tag is
 * what separates them, and answering a range for any buffered body rather than
 * only a file is what makes such a handler reachable.
 */
static void ranges_on_an_ordinary_handler(http::server& s) {
    std::cout << "\nranges on a handler, and If-Range's strong comparison:\n";

    const std::string all = "0123456789abcdefghij";

    {
        const util::http::Response r =
            ask_with(s, "GET", "/strong", "Range", "bytes=2-5");

        ok("  a handler's body can be ranged, not just a file",
           r.status() == 206 && r.body() == all.substr(2, 4),
           std::to_string(r.status()) + " \"" + r.body() + "\"");

        ok("  with a Content-Range over the whole body",
           r.fields().get("Content-Range") == "bytes 2-5/20",
           r.fields().get("Content-Range"));
    }

    {
        const util::http::Response r =
            ask_with(s, "GET", "/strong", "Range", "bytes=100-200");

        ok("  and an unsatisfiable one is 416 here too", r.status() == 416,
           std::to_string(r.status()));
    }

    {
        // A range of something that is not there is not a thing.  Without the
        // status check this would be a 416 about a 404's body, which tells a
        // client its offset was wrong rather than that the path was.
        const util::http::Response r =
            ask_with(s, "GET", "/nowhere", "Range", "bytes=0-3");

        ok("  a Range on a 404 leaves it a 404", r.status() == 404,
           std::to_string(r.status()));
    }

    {
        // **The positive case, which nothing could reach before.**  A strong
        // tag on both sides is what If-Range is for.
        util::http::fields f;

        f.add("Range", "bytes=0-3");
        f.add("If-Range", "\"strong-v1\"");

        util::http::Response r;

        try { r = http::request("GET", util::URL(s.url("/strong")), f); }
        catch(std::exception&) {}

        ok("  a strong If-Range that matches authorises the range",
           r.status() == 206 && r.body() == all.substr(0, 4),
           std::to_string(r.status()) + " \"" + r.body() + "\"");
    }

    {
        // The client's tag is weak, the server's is strong: refused, because
        // weak comparison is not good enough to splice on.
        util::http::fields f;

        f.add("Range", "bytes=0-3");
        f.add("If-Range", "W/\"strong-v1\"");

        util::http::Response r;

        try { r = http::request("GET", util::URL(s.url("/strong")), f); }
        catch(std::exception&) {}

        ok("  a weak tag from the client does not, even against a strong one",
           r.status() == 200 && r.body() == all,
           std::to_string(r.status()) + " \"" + r.body() + "\"");
    }

    {
        util::http::fields f;

        f.add("Range", "bytes=0-3");
        f.add("If-Range", "\"something-else\"");

        util::http::Response r;

        try { r = http::request("GET", util::URL(s.url("/strong")), f); }
        catch(std::exception&) {}

        ok("  and a strong tag that does not match gives the whole body",
           r.status() == 200 && r.body() == all, std::to_string(r.status()));
    }
}

static void a_head_sends_nothing_after_the_head() {
    std::cout << "\na HEAD sends the head and stops:\n";

    http::server s(http::server::async_t(), 0, "127.0.0.1");

    furnish(s);
    s.transport().on_error([](const std::exception&, const sys::peer&) {});

    std::thread t([&s]{ s.run(); });

    {
        sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

        c << "HEAD /exact HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

        // Read the head only -- deliberately not read_body, which would be
        // told this was a HEAD and skip the very octets in question.
        const std::string head = util::http::read_head(c, 8192);
        const util::http::Response r = util::http::parse_head(head, true);

        ok("  it is answered", r.status() == 200, std::to_string(r.status()));

        ok("  and says how long the body would have been",
           r.fields().get("Content-Length") == "5",
           r.fields().get("Content-Length"));

        // Nothing was consumed after the head, so if the server wrote a body
        // it is sitting here and this parse lands inside it.
        c << "GET /second HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

        bool ok_next = false;
        std::string why;

        try {
            const std::string h2 = util::http::read_head(c, 8192);
            const util::http::Response r2 = util::http::parse_head(h2);

            ok_next = r2.status() == 404;
            why = std::to_string(r2.status());
        }
        catch(std::exception& e) { why = e.what(); }

        // /second is not routed here, so 404 is the right answer -- what
        // matters is that it parsed as a response at all.
        ok("  and the next request on the connection parses cleanly",
           ok_next, why);
    }

    s.stop();
    t.join();

    // The blocking server closes after one response, so there is no "next
    // request" to corrupt -- read to the close instead and require silence.
    {
        http::server b(0, "127.0.0.1");

        furnish(b);
        b.transport().on_error([](const std::exception&, const sys::peer&) {});

        std::thread bt([&b]{ b.run(); });

        {
            sys::socketstream c("127.0.0.1", b.port(), -1, 10.0);

            c << "HEAD /exact HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

            const std::string head = util::http::read_head(c, 8192);
            const util::http::Response r = util::http::parse_head(head, true);

            ok("  the blocking server answers a HEAD too", r.status() == 200,
               std::to_string(r.status()));

            std::string after;
            char ch;

            while(c.get(ch)) after += ch;

            ok("  and writes nothing at all after the head", after.empty(),
               "\"" + after + "\"");
        }

        b.stop();
        bt.join();
    }
}

static void a_bad_pattern_is_refused_at_registration() {
    std::cout << "\na pattern that cannot work is refused where it is written:\n";

    http::server s(0, "127.0.0.1");

    struct { const char* pattern; const char* why; } bad[] = {
        { "/a/*/b",   "* before the end" },
        { "/a/{}/b",  "an unnamed parameter" },
        { "/a/x{y}z", "a brace in a literal" },
    };

    for(std::size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        bool threw = false;
        std::string why;

        try {
            s.route("GET", bad[i].pattern,
                    [](const http::server::Request&,
                       http::server::response&) {});
        }
        catch(std::exception& e) { threw = true; why = e.what(); }

        // At registration, where the caller is -- not at match time, where
        // the only symptom would be a route that never fires.
        ok(std::string("  ") + bad[i].pattern + " is refused (" +
           bad[i].why + ")", threw, why);
    }
}

int main() {
    std::cout << "net_http_routing_test\n";

    try {
        {
            http::server s(0, "127.0.0.1");

            furnish(s);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            std::thread t([&s]{ s.run(); });

            run_against(s, "the blocking server");

            s.stop();
            t.join();
        }

        {
            http::server s(http::server::async_t(), 0, "127.0.0.1");

            furnish(s);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            std::thread t([&s]{ s.run(); });

            run_against(s, "the async server");

            s.stop();
            t.join();
        }

        a_head_sends_nothing_after_the_head();
        a_bad_pattern_is_refused_at_registration();
    }
    catch(std::exception& e) {
        std::cerr << "net_http_routing_test: " << e.what() << "\n";
        return 1;
    }

    if(failures) {
        std::cerr << "net_http_routing_test: " << failures << " failed\n";
        return 1;
    }

    std::cout << "net_http_routing_test: all good\n";

    return 0;
}
