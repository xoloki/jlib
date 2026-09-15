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
        // A path that *does* match for this method must not collect an Allow
        // from the other routes it passed on the way.
        const util::http::Response r = ask(s, "POST", "/exact");

        ok("  a method that does match is answered, not refused",
           r.status() == 200 && r.body() == "posted", r.body());

        ok("  and carries no Allow", !r.fields().has("Allow"),
           r.fields().get("Allow"));
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
