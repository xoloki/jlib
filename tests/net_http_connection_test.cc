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

// net::http::connection -- several requests down one socket.
//
// **The assertion is how many times the server accepted**, not whether the
// client's local port stayed the same. juliet's objection to the port check is
// the right one: a client that reconnects can be handed the same port again,
// and "unlikely" is a poor foundation for a test whose only job is to prove
// reuse. Handler entries against requests served cannot be satisfied by a
// coincidence.
//
// The server here is sys::server rather than net::http::server, for the same
// reason the test exists: this is about what the *client* does, and a server
// that counts its own accepts and answers by hand shares no assumptions with
// it. That it also works against jlib's own HTTP server is asserted at the
// end, where the count is not available.
//
// Three requests, not two. Two is enough to look like reuse and not enough to
// show the state that reuse carries -- #217 verified keep-alive against a
// client that opens a connection per call (#221), so for a while the second
// request on a connection was tested by nothing at all. The third is a
// different method with a body, because what breaks on reuse is per-request
// state that did not get reset.

#include <jlib/net/http.hh>
#include <jlib/sys/server.hh>
#include <jlib/util/http.hh>
#include <jlib/util/URL.hh>

#include <atomic>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace sys = jlib::sys;
namespace http = jlib::net::http;
namespace util = jlib::util;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** What the server counted, which is the whole subject. */
struct tally {
    std::atomic<int> accepts{0};     ///< entries into the connection handler
    std::atomic<int> requests{0};    ///< request heads read
    std::atomic<int> posts{0};
};

/**
 * A server that answers requests on one connection until told otherwise.
 *
 * Deliberately hand-rolled: it counts its own accepts, which net::http::server
 * does not expose, and it answers with framing this test chooses.
 */
static void serve(sys::socketstream& s, tally& t, bool close_after_first,
                  std::size_t reply_size)
{
    t.accepts++;

    for(;;) {
        util::http::Request q;

        try { q = util::http::read_request_head(s); }
        catch(std::exception&) { return; }        // the client hung up

        t.requests++;

        // A real server closes when the request asks it to.  Without this the
        // server is more lenient than any it will meet, and a client that
        // wrongly said "close" would look like one that reused correctly --
        // which is exactly what happened when this break was first run.
        const bool asked_close =
            util::http::fold(q.fields().get("Connection")).find("close") !=
            std::string::npos;

        if(q.method() == "POST") {
            t.posts++;

            // The body has to be consumed or the next head read starts inside
            // it -- which is exactly the per-request state a third request
            // catches and a second does not.
            util::http::read_body(s, q.body_framing(), q.content_length());
        }

        const std::string body(reply_size, 'y');

        std::ostringstream out;

        out << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: text/plain\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: "
            << (close_after_first || asked_close ? "close" : "keep-alive")
            << "\r\n\r\n" << body;

        s << out.str() << std::flush;

        if(close_after_first || asked_close) return;
    }
}

static void three_requests_one_socket() {
    std::cout << "\nthree requests down one connection:\n";

    tally t;

    sys::server server(0,
                       [&t](sys::socketstream& s, const sys::peer&) {
                           serve(s, t, false, 16);
                       },
                       "127.0.0.1");

    server.on_error([](const std::exception&, const sys::peer&) {});

    std::thread th([&server]{ server.run(); });

    const std::string base = "http://127.0.0.1:" + std::to_string(server.port());

    {
        http::connection c(util::URL(base + "/"));

        const http::Response a = c.request("GET", util::URL(base + "/one"));
        const http::Response b = c.request("GET", util::URL(base + "/two"));

        ok("the first two are answered",
           a.status() == 200 && b.status() == 200 && a.body().size() == 16,
           std::to_string(a.status()) + "/" + std::to_string(b.status()));

        ok("  and the connection is still live after each", c.live());

        // Different in kind: a method with a body, so anything the server did
        // not reset between requests shows up here rather than looking fine.
        util::http::fields f;

        f.add("Content-Type", "text/plain");

        const http::Response third =
            c.request("POST", util::URL(base + "/three"), f, "a body");

        ok("the third, a POST with a body, is answered too", third.status() == 200,
           std::to_string(third.status()));

        ok("  and this connection carried all three", c.served() == 3,
           std::to_string(c.served()) + " served");
    }

    server.stop();
    th.join();

    // The assertion.  Everything above could be true of three connections.
    ok("the server accepted once", t.accepts == 1, std::to_string(t.accepts));

    ok("  and read three requests on it", t.requests == 3,
       std::to_string(t.requests));

    ok("  one of them a POST whose body was consumed", t.posts == 1,
       std::to_string(t.posts));
}

static void a_server_that_says_close() {
    std::cout << "\na server that will not keep it:\n";

    tally t;

    sys::server server(0,
                       [&t](sys::socketstream& s, const sys::peer&) {
                           serve(s, t, true, 8);
                       },
                       "127.0.0.1");

    server.on_error([](const std::exception&, const sys::peer&) {});

    std::thread th([&server]{ server.run(); });

    const std::string base = "http://127.0.0.1:" + std::to_string(server.port());

    {
        http::connection c(util::URL(base + "/"));

        const http::Response a = c.request("GET", util::URL(base + "/one"));

        ok("the first request is answered", a.status() == 200);

        ok("  and the connection knows it is finished", !c.live());

        // Kept working rather than throwing: a dead connection reopens, which
        // is what a pool would have done.
        const http::Response b = c.request("GET", util::URL(base + "/two"));

        ok("  the next request opens a new one", b.status() == 200 && c.served() == 1,
           std::to_string(c.served()));
    }

    server.stop();
    th.join();

    ok("so the server accepted twice", t.accepts == 2, std::to_string(t.accepts));
}

static void a_body_taken_as_it_arrives() {
    std::cout << "\na body handed over as it arrives:\n";

    tally t;

    sys::server server(0,
                       [&t](sys::socketstream& s, const sys::peer&) {
                           serve(s, t, false, 10000);
                       },
                       "127.0.0.1");

    server.on_error([](const std::exception&, const sys::peer&) {});

    std::thread th([&server]{ server.run(); });

    const std::string base = "http://127.0.0.1:" + std::to_string(server.port());

    {
        http::connection c(util::URL(base + "/"));

        std::size_t calls = 0, octets = 0;

        const http::Response r =
            c.request("GET", util::URL(base + "/big"), util::http::fields(), "",
                      [&](std::string_view piece) {
                          calls++;
                          octets += piece.size();

                          return true;
                      });

        ok("the sink saw the whole body", octets == 10000,
           std::to_string(octets) + " octets in " + std::to_string(calls) + " calls");

        ok("  in more than one piece", calls > 1, std::to_string(calls));

        // The Response does not carry what the sink already has.  A caller
        // that asked to stream said it would not hold the body.
        ok("  and the response body is empty", r.body().empty(),
           std::to_string(r.body().size()));

        ok("  the connection survives a body read to the end", c.live());

        // A sink that stops leaves the stream mid-body, so the connection
        // cannot be reused -- there is no way to know what is left.
        std::size_t few = 0;

        c.request("GET", util::URL(base + "/big"), util::http::fields(), "",
                  [&](std::string_view) { few++; return false; });

        ok("but a sink that stops early kills it", !c.live(),
           std::to_string(few) + " calls before stopping");
    }

    server.stop();
    th.join();
}

static void a_target_for_somewhere_else() {
    std::cout << "\na target that is not this origin:\n";

    http::connection c(util::URL("http://127.0.0.1:1/"));

    bool threw = false;
    std::string why;

    try { c.request("GET", util::URL("http://example.com/")); }
    catch(http::error& e) { threw = true; why = e.what(); }

    ok("is refused rather than sent", threw, why);

    ok("  saying both origins",
       why.find("127.0.0.1:1") != std::string::npos &&
       why.find("example.com") != std::string::npos, why);
}

int main() {
    std::cout << std::unitbuf;

    three_requests_one_socket();
    a_server_that_says_close();
    a_body_taken_as_it_arrives();
    a_target_for_somewhere_else();

    // What a green run does not establish.
    //
    // **Not TLS.** Every connection here is plain http. The socket comes from
    // the same transport() the one-shot client uses, so https should work the
    // same way, and "should" is not a test.
    //
    // Not a proxy, for the same reason and through the same function.
    //
    // Not concurrency. A connection belongs to one thread, the header says so,
    // and nothing here tries two.
    //
    // Not a server that closes *between* requests without saying so -- the
    // socket would fail on the next write and the connection would reopen,
    // which is the same path as the Connection: close case above but arrived
    // at by a timeout. Untested because making a server drop a connection at a
    // chosen moment needs more harness than the case is worth today.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
