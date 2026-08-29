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

// jlib's HTTP client against jlib's HTTP server.
//
// Both halves of a protocol this library has only ever spoken one end of, in
// one process, over plaintext and over TLS.  Which is a strong test and a
// dangerous one: two implementations by the same hand agree about things a
// stranger would not, so net_http_live_test keeps driving the client at a real
// nginx and no live test is converted to this shape.

#include "certificate.hh"

#include <jlib/net/http.hh>
#include <jlib/net/http_server.hh>

#include <jlib/sys/socketstream.hh>
#include <jlib/sys/tls.hh>

#include <jlib/util/URL.hh>

#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

namespace http = jlib::net::http;

using jlib::util::URL;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Wire up a server with the routes every section here uses. */
static void furnish(http::server& s, std::string* saw_body = 0,
                    std::string* saw_target = 0)
{
    s.route("GET", "/hello", [](const http::server::Request&,
                                http::server::response& r) {
        r.status(200).type("text/plain").body("hello from jlib\n");
    });

    s.route("GET", "/echo", [saw_target](const http::server::Request& q,
                                         http::server::response& r) {
        if(saw_target) *saw_target = q.target();

        r.status(200).type("text/plain").body(q.target());
    });

    s.route("POST", "/form", [saw_body](const http::server::Request& q,
                                        http::server::response& r) {
        if(saw_body) *saw_body = q.body();

        r.status(200).type("application/json").body("{\"got\":true}");
    });

    s.route("GET", "/boom", [](const http::server::Request&,
                               http::server::response&) {
        throw std::runtime_error("the handler gave up");
    });

    s.route("GET", "/bad-field", [](const http::server::Request&,
                                    http::server::response& r) {
        // A CR in a value is how one response becomes two.  Whatever a handler
        // was given, this must not reach the wire.
        r.status(200).field("X-Thing", "one\r\nX-Injected: two").body("x");
    });
}

/** One request, on a thread, while the server serves one. */
static http::Response exchange(http::server& s, const std::string& method,
                               const std::string& path,
                               const std::map<std::string, std::string>& form =
                                   std::map<std::string, std::string>())
{
    http::Response got;
    std::string trouble;

    std::thread client([&] {
        try {
            if(method == "POST") got = http::post_form(URL(s.url(path)), form);
            else                 got = http::get(URL(s.url(path)));
        }
        catch(std::exception& e) { trouble = e.what(); }
    });

    try { s.serve_one(10); }
    catch(std::exception&) {}

    client.join();

    if(!trouble.empty() && got.status() == 0)
        std::cout << "         (client: " << trouble << ")\n";

    return got;
}

static void the_two_halves_meet() {
    std::cout << "\nthe two halves meet:\n";

    http::server s;

    furnish(s);

    ok("the server bound a port", s.port() != 0, std::to_string(s.port()));
    ok("and it is plaintext", !s.tls());

    const http::Response r = exchange(s, "GET", "/hello");

    ok("a routed GET is answered", r.status() == 200 && r.body() == "hello from jlib\n",
       r.body());

    // Fields the server supplies unless a handler said otherwise.
    ok("with a Date, a Server and a Content-Length",
       r.fields().has("Date") && r.fields().has("Server") &&
       r.content_length() == r.body().size());

    // Always, so no keep-alive framing question ever arises.
    ok("and Connection: close",
       jlib::util::http::fold(r.fields().get("Connection")) == "close",
       r.fields().get("Connection"));

    const http::Response missing = exchange(s, "GET", "/nowhere");

    ok("an unrouted path gets the fallback", missing.status() == 404,
       std::to_string(missing.status()));

    // Exact matching, in the order routes were added.  A GET route does not
    // answer a POST.
    const http::Response wrong = exchange(s, "POST", "/hello");

    ok("and a method that was not routed does too", wrong.status() == 404,
       std::to_string(wrong.status()));
}

static void what_the_handler_sees() {
    std::cout << "\nwhat the handler sees:\n";

    std::string body, target;

    http::server s;

    furnish(s, &body, &target);

    const http::Response r = exchange(s, "GET", "/echo?a=1&b=two");

    // The query reaches the handler whole, and does not affect routing --
    // /echo?a=1 routes on /echo.
    ok("the target arrives with its query", target == "/echo?a=1&b=two", target);
    ok("and routing ignored the query", r.status() == 200);

    const http::Response posted = exchange(s, "POST", "/form",
                                           { { "grant_type", "refresh_token" } });

    ok("a form body arrives whole",
       body == "grant_type=refresh_token", body);
    ok("and the handler's answer comes back",
       posted.status() == 200 && posted.body() == "{\"got\":true}", posted.body());
}

static void what_it_refuses() {
    std::cout << "\nwhat it refuses:\n";

    http::server s;

    furnish(s);

    // Something that is not a request at all.  A bare close would be
    // indistinguishable from a crash, so it answers and stops.
    {
        std::string reply;

        std::thread client([&s, &reply] {
            try {
                jlib::sys::socketstream c("127.0.0.1", s.port(), 5);

                c.set_timeout(5);
                c << "GET\r\n\r\n" << std::flush;

                std::getline(c, reply);
            }
            catch(std::exception&) {}
        });

        s.serve_one(10);
        client.join();

        ok("a malformed request line gets a 400",
           reply.find("400") != std::string::npos, reply);
    }

    // And the server is still serving, which is the point of answering rather
    // than dropping.
    const http::Response after = exchange(s, "GET", "/hello");

    ok("and the server carries on", after.status() == 200);

    // An encoded separator is refused rather than decoded: decoding one would
    // change how many segments the path has.
    {
        std::string reply;

        std::thread client([&s, &reply] {
            try {
                jlib::sys::socketstream c("127.0.0.1", s.port(), 5);

                c.set_timeout(5);
                c << "GET /a%2F..%2Fb HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

                std::getline(c, reply);
            }
            catch(std::exception&) {}
        });

        s.serve_one(10);
        client.join();

        ok("an encoded path separator is refused",
           reply.find("400") != std::string::npos, reply);
    }

    // A handler that throws is answered with a 500, because nothing has reached
    // the socket until it returns -- which is why a response is accumulated.
    int reported = 0;

    s.transport().on_error([&reported](const std::exception&, const jlib::sys::peer&) {
        reported++;
    });

    const http::Response boom = exchange(s, "GET", "/boom");

    ok("a handler that throws still answers, with a 500", boom.status() == 500,
       std::to_string(boom.status()));
    ok("and the failure is reported rather than swallowed", reported == 1,
       std::to_string(reported));

    // A field a handler built from something it was given.  Checked against the
    // grammar on the way out, so it cannot become two responses.
    const http::Response bad = exchange(s, "GET", "/bad-field");

    // 500, not a bare close: serialising the response happens inside the same
    // try that answers, so a field the server refuses to send is reported to
    // the client as the server's fault rather than as a dropped connection.
    ok("a header value containing CRLF never reaches the wire, and says so",
       bad.status() == 500, std::to_string(bad.status()));
    ok("and that failure is reported too", reported == 2,
       std::to_string(reported));
}

static void over_tls() {
    std::cout << "\nover TLS:\n";

    const std::string cert = "http_server_cert.pem";
    const std::string key = "http_server_key.pem";

    if(!make_cert(cert, key)) {
        std::cout << "  skip  could not generate a test certificate\n";

        return;
    }

    const char* const had = std::getenv("SSL_CERT_FILE");
    const std::string keep = had ? had : "";

    ::setenv("SSL_CERT_FILE", cert.c_str(), 1);

    try {
        http::server s(0, "127.0.0.1", jlib::sys::tls_context::server(cert, key));

        furnish(s);

        ok("it is an https:// server", s.tls() &&
           s.url("/hello").compare(0, 8, "https://") == 0, s.url("/hello"));

        const http::Response r = exchange(s, "GET", "/hello");

        ok("and the client reaches it over TLS",
           r.status() == 200 && r.body() == "hello from jlib\n", r.body());

        const http::Response posted = exchange(s, "POST", "/form",
                                               { { "a", "b" } });

        ok("a POST works over TLS too", posted.status() == 200, posted.body());

        const http::Response missing = exchange(s, "GET", "/nowhere");

        ok("and so does the 404 fallback", missing.status() == 404,
           std::to_string(missing.status()));
    }
    catch(std::exception& e) {
        ok("an https:// server runs", false, e.what());
    }

    if(keep.empty()) ::unsetenv("SSL_CERT_FILE");
    else             ::setenv("SSL_CERT_FILE", keep.c_str(), 1);

    std::remove(cert.c_str());
    std::remove(key.c_str());
}

int main() {
    std::cout << std::unitbuf;

    the_two_halves_meet();
    what_the_handler_sees();
    what_it_refuses();
    over_tls();

    // What a green run does not establish.
    //
    // Interoperability, and this is the section to read twice.  Both ends here
    // were written by the same hand, from the same grammar, in the same week --
    // so they agree about things a stranger would not, and a disagreement
    // between jlib and the rest of the world would show up as two green ticks.
    // net_http_live_test drives the client at a real nginx for exactly that
    // reason, and nothing here replaces it.
    //
    // Not keep-alive, cookies, compression, ranges, or a body that does not fit
    // in memory.  None of those is implemented and none is a gap -- see the
    // header.
    //
    // Not concurrency.  Every section serves one connection at a time;
    // sys_server_test is where the threaded policy is exercised.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
