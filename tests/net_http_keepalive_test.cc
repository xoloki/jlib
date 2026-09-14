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
/*
 * net_http_keepalive_test -- more than one request on one connection.
 *
 * Keep-alive was refused outright until now, and the reason given was that its
 * framing is where request smuggling lives.  So most of this file is not about
 * the feature: it is about the boundary.  A connection that is reused is a
 * connection on which **the server and the client must agree, octet for octet,
 * where one message stopped** -- and every way of disagreeing is a way for a
 * client to make the tail of its own request look like the head of somebody
 * else's.
 *
 * Every section here that says "and the connection closes" is load-bearing.
 * The ones about answering two requests would pass on a server with no
 * defences at all.
 */

#include "certificate.hh"

#include <jlib/net/http.hh>
#include <jlib/net/http_server.hh>
#include <jlib/sys/sslstream.hh>
#include <jlib/sys/socketstream.hh>
#include <jlib/util/http.hh>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
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

static void furnish(http::server& s) {
    s.route("GET", "/hello", [](const http::server::Request&,
                                http::server::response& r) {
        r.status(200).type("text/plain").body("hello\n");
    });

    s.route("GET", "/second", [](const http::server::Request&,
                                 http::server::response& r) {
        r.status(200).type("text/plain").body("second\n");
    });

    s.route("POST", "/form", [](const http::server::Request& q,
                                http::server::response& r) {
        r.status(200).type("text/plain").body(q.body());
    });
}

/**
 * Read exactly one response and stop at its last octet.
 *
 * The client's own framing code, deliberately: if this and the server
 * disagreed about where a message ends, that disagreement is the bug being
 * looked for, and a hand-rolled reader here would hide it.
 */
static bool reply(std::istream& is, util::http::Response& r) {
    try {
        r = util::http::parse_head(util::http::read_head(is, 8192));

        // until_close would sit here until the server hung up, which is the
        // opposite of what a kept-alive response does -- so a response framed
        // that way is a failure of this test's premise, not a body to read.
        if(r.body_framing() != util::http::framing::length) return false;

        r.set_body(util::http::read_body(is, r, 1 << 20));

        return true;
    }
    catch(std::exception&) { return false; }
}

static std::string connection_of(const util::http::Response& r) {
    return util::http::fold(r.fields().get("Connection"));
}

/** Is the peer gone?  A read that ends rather than blocking says so. */
static bool closed(std::istream& is) {
    return is.get() == std::char_traits<char>::eof();
}

// ---------------------------------------------------------------------------

static void two_requests_on_one_connection() {
    std::cout << "\ntwo requests on one connection:\n";

    http::server s(http::server::async_t(), 0, "127.0.0.1");

    furnish(s);
    s.transport().on_error([](const std::exception&, const sys::peer&) {});

    std::thread t([&s]{ s.run(); });

    {
        sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

        c << "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

        util::http::Response first;

        ok("  the first is answered", reply(c, first) && first.status() == 200,
           first.body());

        ok("  and says the connection stays open",
           connection_of(first) == "keep-alive", connection_of(first));

        // The whole feature, in one line: nothing reconnected.
        c << "GET /second HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

        util::http::Response second;

        const bool got = reply(c, second);

        ok("  the second is answered on the same connection",
           got && second.status() == 200, second.body());

        ok("  and it is the second route, not the first repeated",
           got && second.body() == "second\n", second.body());
    }

    s.stop();
    t.join();
}

static void a_pipelined_pair() {
    std::cout << "\na pipelined pair, both requests in one write:\n";

    http::server s(http::server::async_t(), 0, "127.0.0.1");

    furnish(s);
    s.transport().on_error([](const std::exception&, const sys::peer&) {});

    std::thread t([&s]{ s.run(); });

    {
        sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

        // **The test the buffering has to survive.**  Both heads arrive in one
        // read(2), so the second one is already sitting in the connection's
        // async_reader when the first is answered.  A server that went back to
        // the descriptor for the next request -- rather than to the buffer it
        // already filled -- would hang here until the deadline.
        c << "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n"
             "GET /second HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

        util::http::Response first, second;

        const bool a = reply(c, first);
        const bool b = reply(c, second);

        ok("  both are answered", a && b,
           std::to_string(first.status()) + " then " +
           std::to_string(second.status()));

        ok("  in the order they were asked",
           a && b && first.body() == "hello\n" && second.body() == "second\n",
           first.body() + " then " + second.body());
    }

    s.stop();
    t.join();
}

static void a_body_is_consumed_whole() {
    std::cout << "\na request body is consumed whole:\n";

    http::server s(http::server::async_t(), 0, "127.0.0.1");

    furnish(s);
    s.transport().on_error([](const std::exception&, const sys::peer&) {});

    std::thread t([&s]{ s.run(); });

    {
        sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

        // **The smuggle a forgetful server performs on itself.**  If any octet
        // of this body were left in the reader, the next read_head would begin
        // at it -- and the body is chosen to be a valid request line, so the
        // server would answer a request the client never sent as though it
        // had.  The assertion is not that /form works; it is that what follows
        // is /second and not /smuggled.
        const std::string body = "GET /smuggled HTTP/1.1\r\nHost: x\r\n\r\n";

        c << "POST /form HTTP/1.1\r\nHost: x\r\n"
          << "Content-Length: " << body.size() << "\r\n\r\n"
          << body << std::flush;

        util::http::Response posted;

        ok("  the POST is answered with its own body",
           reply(c, posted) && posted.body() == body, posted.body());

        c << "GET /second HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

        util::http::Response next;

        const bool got = reply(c, next);

        ok("  and the next request is the one the client sent",
           got && next.body() == "second\n", next.body());
    }

    s.stop();
    t.join();
}

static void the_client_can_still_say_close() {
    std::cout << "\na client that asks for a close gets one:\n";

    http::server s(http::server::async_t(), 0, "127.0.0.1");

    furnish(s);
    s.transport().on_error([](const std::exception&, const sys::peer&) {});

    std::thread t([&s]{ s.run(); });

    {
        sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

        c << "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
          << std::flush;

        util::http::Response r;

        ok("  it is answered", reply(c, r) && r.status() == 200, r.body());
        ok("  and told so", connection_of(r) == "close", connection_of(r));
        ok("  and the connection really closes", closed(c));
    }

    {
        // Not one value, a list -- RFC 9110 7.6.1.  A server comparing the
        // whole field to "close" would keep this one open.
        sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

        c << "GET /hello HTTP/1.1\r\nHost: x\r\n"
             "Connection: keep-alive, close\r\n\r\n" << std::flush;

        util::http::Response r;

        ok("  a close listed among other options is still a close",
           reply(c, r) && connection_of(r) == "close", connection_of(r));
        ok("  and that connection closes too", closed(c));
    }

    {
        // HTTP/1.0 is not persistent, and jlib does not implement its
        // keep-alive extension -- see util::http::persistent for the proxy
        // reason why not.
        sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

        c << "GET /hello HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n"
          << std::flush;

        util::http::Response r;

        ok("  an HTTP/1.0 request gets a close even when it asks otherwise",
           reply(c, r) && connection_of(r) == "close", connection_of(r));
        ok("  and that connection closes as well", closed(c));
    }

    s.stop();
    t.join();
}

static void a_framing_error_closes() {
    std::cout << "\na message whose end is arguable closes the connection:\n";

    http::server s(http::server::async_t(), 0, "127.0.0.1");

    furnish(s);
    s.transport().on_error([](const std::exception&, const sys::peer&) {});

    std::thread t([&s]{ s.run(); });

    {
        // **The classic CL.TE smuggle.**  util::http refuses the message, and
        // this is the half that lives up here: having said it cannot work out
        // where this request ended, the server must not then go looking for
        // the next one.  The octets after the head are a complete request, and
        // the assertion is that they are never answered.
        sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

        c << "POST /form HTTP/1.1\r\nHost: x\r\n"
             "Content-Length: 6\r\nTransfer-Encoding: chunked\r\n\r\n"
             "0\r\n\r\nGET /smuggled HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

        util::http::Response r;

        ok("  it is refused with a 400", reply(c, r) && r.status() == 400,
           std::to_string(r.status()) + " " + r.body());

        ok("  the refusal says where the message ends is in doubt",
           r.body().find("depends on who is reading it") != std::string::npos,
           r.body());

        ok("  the connection closes rather than reading on", closed(c));
    }

    {
        // The same rule for a head that is merely unparseable: the reason it
        // could not be parsed is not known to be unrelated to its length.
        sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

        c << "GET /hello HTTP/1.1\r\nHost : x\r\n\r\n"
             "GET /second HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

        util::http::Response r;

        ok("  whitespace before a colon is a 400 too",
           reply(c, r) && r.status() == 400, std::to_string(r.status()));
        ok("  and closes", closed(c));
    }

    s.stop();
    t.join();
}

static void the_bounds() {
    std::cout << "\nwhat bounds a connection that never stops asking:\n";

    {
        http::server::options o;

        o.max_requests = 3;

        http::server s(http::server::async_t(), 0, "127.0.0.1",
                       sys::tls_context(), sys::server::policy(), o);

        furnish(s);
        s.transport().on_error([](const std::exception&, const sys::peer&) {});

        std::thread t([&s]{ s.run(); });

        {
            sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

            int answered = 0;
            std::string last;

            for(int i = 0; i < 5; i++) {
                c << "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

                util::http::Response r;

                if(!reply(c, r)) break;

                ++answered;
                last = connection_of(r);
            }

            ok("  max_requests is what stops it, and at the number given",
               answered == 3, std::to_string(answered) + " answered");

            ok("  the last one says the connection is ending", last == "close",
               last);

            ok("  and it does", closed(c));
        }

        s.stop();
        t.join();
    }

    {
        http::server::options o;

        o.idle_timeout = 0.3;

        http::server s(http::server::async_t(), 0, "127.0.0.1",
                       sys::tls_context(), sys::server::policy(), o);

        furnish(s);
        s.transport().on_error([](const std::exception&, const sys::peer&) {});

        std::thread t([&s]{ s.run(); });

        {
            sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

            c << "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

            util::http::Response r;

            ok("  a connection that is answered stays open",
               reply(c, r) && connection_of(r) == "keep-alive",
               connection_of(r));

            // **Why the bound exists.**  No thread is held here -- that is
            // what makes keep-alive affordable at all -- but a descriptor is,
            // and so is a slot against max_connections.  Without this an idle
            // client is how a server stops accepting.
            const auto began = std::chrono::steady_clock::now();

            const bool went = closed(c);

            const double waited = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - began).count();

            ok("  and an idle one is closed by idle_timeout", went,
               std::to_string(waited) + "s");

            // One-sided: a timer that fired late is slow, a timer that fired
            // early is wrong.  Only the second can be asserted without flaking.
            ok("  not before the timeout it was given", waited >= 0.25,
               std::to_string(waited) + "s");
        }

        s.stop();
        t.join();
    }
}

static void a_streaming_response_still_closes() {
    std::cout << "\na streaming response still ends at the close:\n";

    // An idle_timeout far longer than this section can take, so that "the
    // connection closed" cannot be confused with "the connection was reaped".
    // A server that kept a streaming connection alive would sit here for
    // thirty seconds; a correct one closes as the handler returns.
    http::server::options o;

    o.idle_timeout = 30;

    http::server s(http::server::async_t(), 0, "127.0.0.1",
                   sys::tls_context(), sys::server::policy(), o);

    s.route("GET", "/stream",
            [](const http::server::Request&,
               http::server::async_responder& out) -> sys::task<void> {
                http::server::response head;

                head.status(200).type("text/event-stream");

                co_await out.begin(head);
                co_await out.write("data: one\n\n");
            });

    s.transport().on_error([](const std::exception&, const sys::peer&) {});

    std::thread t([&s]{ s.run(); });

    {
        sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

        c << "GET /stream HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

        const std::string head = util::http::read_head(c, 8192);
        const util::http::Response r = util::http::parse_head(head);

        ok("  it answers 200", r.status() == 200, std::to_string(r.status()));

        ok("  with no Content-Length, because it was not known",
           !r.fields().has("Content-Length"));

        // The reason it cannot be kept alive, stated on the wire: there is no
        // length, so the close *is* the framing, so there is no boundary for a
        // next request to begin at.  Chunked output is what would change this
        // and there is none.
        ok("  and says close, whatever keep_alive is set to",
           connection_of(r) == "close", connection_of(r));

        const auto began = std::chrono::steady_clock::now();

        const std::string body = util::http::read_body(c, r, 1 << 20);

        const double waited = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - began).count();

        ok("  the body arrives and the close delimits it",
           body == "data: one\n\n", body);

        // **The assertion the Connection field cannot make.**  begin() writes
        // "close" into the head whatever the server then does, so a server
        // that looped after a streaming response would still look correct on
        // the wire -- and would be found only by the client waiting out an
        // idle_timeout for a body that had already finished.
        ok("  and the close is the handler returning, not a timeout expiring",
           waited < 3.0, std::to_string(waited) + "s of a 30s idle_timeout");
    }

    s.stop();
    t.join();
}

/**
 * Connect a raw descriptor, so the write side can be closed on its own.
 *
 * socketstream cannot half-close, and a half-close is the only way to *watch*
 * what a server does about a goodbye: close both directions and anything it
 * sends lands on a dead socket and disappears, which is exactly how this went
 * unnoticed the first time it was tested.
 */
static int connect_raw(unsigned short port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if(fd < 0) return -1;

    struct sockaddr_in a;

    std::memset(&a, 0, sizeof a);

    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if(::connect(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof a) < 0) {
        ::close(fd);
        return -1;
    }

    // So a server that never answers fails this test rather than wedging it.
    struct timeval tv;

    tv.tv_sec = 5;
    tv.tv_usec = 0;

    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    return fd;
}

static void the_polite_goodbye() {
    std::cout << "\na client that just goes away:\n";

    http::server s(http::server::async_t(), 0, "127.0.0.1");

    furnish(s);

    std::atomic<int> errors(0);

    s.transport().on_error([&errors](const std::exception&, const sys::peer&) {
        ++errors;
    });

    std::thread t([&s]{ s.run(); });

    const int fd = connect_raw(s.port());

    if(fd < 0) {
        ok("  a connection is made", false, "connect failed");
        s.stop();
        t.join();
        return;
    }

    const std::string req = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";

    ok("  the request is sent",
       ::write(fd, req.data(), req.size()) == static_cast<ssize_t>(req.size()));

    std::string got;
    char buf[4096];

    while(got.find("hello\n") == std::string::npos) {
        const ssize_t n = ::read(fd, buf, sizeof buf);

        if(n <= 0) break;

        got.append(buf, static_cast<std::size_t>(n));
    }

    ok("  its request is answered",
       got.find("200 OK") != std::string::npos &&
       got.find("hello\n") != std::string::npos, got.substr(0, 40));

    // **The goodbye**, which a server that closes after one response never had
    // to recognise: the client will send no more requests, and says so the way
    // TCP says it -- while still listening, so that whatever the server does
    // next is visible rather than discarded.
    //
    // read_head calls this a message head that ended after zero octets.  It is
    // not: it is a client that finished.
    ::shutdown(fd, SHUT_WR);

    std::string after;

    for(;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);

        if(n <= 0) break;

        after.append(buf, static_cast<std::size_t>(n));
    }

    // Anything at all here is the server answering a request nobody made --
    // in practice a 400 complaining that a head ended early, sent to a client
    // whose only remaining question is whether it may close.
    ok("  a goodbye is answered with silence, then a close", after.empty(),
       after.substr(0, 60));

    ok("  and is not reported as a failure", errors.load() == 0,
       std::to_string(errors.load()) + " reported");

    ::close(fd);

    s.stop();
    t.join();
}

static const std::string KA_GET =
    "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
static const std::string KA_BYE =
    "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";

static std::unique_ptr<std::iostream> open_stream(bool tls, unsigned short port) {
    if(tls) {
        return std::unique_ptr<std::iostream>(
            new sys::tlsstream("localhost", port, false, 10.0, 10.0));
    }

    return std::unique_ptr<std::iostream>(
        new sys::socketstream("127.0.0.1", port, -1, 10.0));
}

/**
 * Seconds for n requests, or -1 if any of them was not answered.
 *
 * Minimum of three runs, never the mean: a scheduler hiccup can only make a
 * run slower, so the fastest is the least contaminated by everything else on
 * the machine.
 */
static double measure(bool tls, unsigned short port, int n, bool reuse) {
    double best = -1;

    for(int run = 0; run < 3; run++) {
        const auto began = std::chrono::steady_clock::now();

        int got = 0;

        try {
            std::unique_ptr<std::iostream> c;

            if(reuse) c = open_stream(tls, port);

            for(int i = 0; i < n; i++) {
                if(!reuse) c = open_stream(tls, port);

                *c << (reuse ? KA_GET : KA_BYE) << std::flush;

                util::http::Response r;

                if(!reply(*c, r) || r.status() != 200) break;

                ++got;
            }
        }
        catch(std::exception&) {}

        const double took = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - began).count();

        if(got == n && (best < 0 || took < best)) best = took;
    }

    return best;
}

static std::string per(double total, int n) {
    if(total < 0) return "n/a";

    char buf[64];

    std::snprintf(buf, sizeof buf, "%.3fms/req", total * 1000.0 / n);

    return buf;
}

/**
 * What a reused connection actually costs and saves, as numbers.
 *
 * Four measurements rather than one, because the interesting question is not
 * "is keep-alive faster" -- it is **which of two opposite effects wins**:
 *
 *   - a reused connection *saves* a handshake, which for TLS is asymmetric
 *     cryptography and several round trips, paid per connection
 *   - a reused connection *costs* a return trip through the reactor: after the
 *     response is written the coroutine suspends and waits for the next
 *     request, where a fresh connection arrives with its request already in
 *     hand
 *
 * Plain HTTP isolates the second, because there is no handshake worth the
 * name.  On loopback, where a round trip is free and a TCP handshake is a
 * formality between two processes on one machine, the second effect can win --
 * and does, on some platforms.  **That is a property of the measurement, not
 * of keep-alive**: over a real network each avoided handshake is several RTTs,
 * and no reactor hop competes with that.
 *
 * So this asserts only what is true everywhere -- that every request was
 * answered, both ways, over both transports -- and prints the rest.  An
 * assertion that reuse is faster would be asserting the properties of the
 * loopback interface it happens to be running on.
 */
static void what_it_costs_and_saves() {
    std::cout << "\nwhat a reused connection costs and saves:\n";

    const std::string cert = "http_keepalive_cert.pem";
    const std::string key = "http_keepalive_key.pem";

    const bool have_cert = make_cert(cert, key);

    const char* const had = std::getenv("SSL_CERT_FILE");
    const std::string keep = had ? had : "";

    if(have_cert) ::setenv("SSL_CERT_FILE", cert.c_str(), 1);

    const int N = 30;

    try {
        http::server plain(http::server::async_t(), 0, "127.0.0.1");

        furnish(plain);
        plain.transport().on_error([](const std::exception&, const sys::peer&) {});

        std::thread pt([&plain]{ plain.run(); });

        const double plain_reused = measure(false, plain.port(), N, true);
        const double plain_fresh  = measure(false, plain.port(), N, false);

        ok("  every plain request was answered, reused and fresh",
           plain_reused >= 0 && plain_fresh >= 0,
           per(plain_reused, N) + " reused, " + per(plain_fresh, N) + " fresh");

        std::cout << "         plain:  " << per(plain_reused, N) << " reused, "
                  << per(plain_fresh, N) << " fresh\n";

        plain.stop();
        pt.join();

        if(!have_cert) {
            std::cout << "  skip   the TLS half: no test certificate\n";
        }
        else {
            http::server s(http::server::async_t(), 0, "127.0.0.1",
                           sys::tls_context::server(cert, key));

            furnish(s);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            std::thread t([&s]{ s.run(); });

            const double tls_reused = measure(true, s.port(), N, true);
            const double tls_fresh  = measure(true, s.port(), N, false);

            ok("  every TLS request was answered, reused and fresh",
               tls_reused >= 0 && tls_fresh >= 0,
               per(tls_reused, N) + " reused, " + per(tls_fresh, N) + " fresh");

            std::cout << "         TLS:    " << per(tls_reused, N) << " reused, "
                      << per(tls_fresh, N) << " fresh\n";

            // The handshake, priced: what a fresh TLS connection costs over a
            // fresh plain one.  This is the thing keep-alive removes, and it
            // is the only figure here that is about TLS rather than about the
            // loopback interface.
            if(tls_fresh >= 0 && plain_fresh >= 0) {
                std::cout << "         a TLS handshake costs "
                          << per(tls_fresh - plain_fresh, N)
                          << ", and a reused connection pays it once\n";
            }

            s.stop();
            t.join();
        }
    }
    catch(std::exception& e) {
        ok("  the servers run", false, e.what());
    }

    if(keep.empty()) ::unsetenv("SSL_CERT_FILE");
    else ::setenv("SSL_CERT_FILE", keep.c_str(), 1);

    if(have_cert) {
        ::remove(cert.c_str());
        ::remove(key.c_str());
    }
}

/**
 * Three bounds, because waiting means three different things.
 *
 *   a new connection, silent        initial_idle_timeout
 *   a request, once it has started  io_timeout
 *   a reused connection, between    idle_timeout
 *
 * Each section here fails if any two of them are collapsed into one, which is
 * the whole point: an earlier draft used a single timer per request, and the
 * third section is the one that caught what that cost.
 */
static void three_bounds_three_questions() {
    std::cout << "\nthree bounds, because waiting means three things:\n";

    // ---- a new connection that says nothing ------------------------------
    {
        http::server::options o;

        o.initial_idle_timeout = 0.3;

        sys::server::policy p;

        p.io_timeout = 5;                       // far longer, and not the one

        http::server s(http::server::async_t(), 0, "127.0.0.1",
                       sys::tls_context(), p, o);

        furnish(s);
        s.transport().on_error([](const std::exception&, const sys::peer&) {});

        std::thread t([&s]{ s.run(); });

        {
            sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

            const auto began = std::chrono::steady_clock::now();

            const bool went = closed(c);

            const double waited = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - began).count();

            ok("  a new connection that says nothing is dropped", went,
               std::to_string(waited) + "s");

            // One-sided, and this is the direction that cannot flake: a timer
            // that fired late is a slow machine, one that fired early is wrong.
            ok("  not before initial_idle_timeout", waited >= 0.25,
               std::to_string(waited) + "s");

            // And this is what says *which* bound did it.  io_timeout is five
            // seconds here; anything under three could not have been it.
            ok("  and by that rather than by io_timeout", waited < 3.0,
               std::to_string(waited) + "s of a 5s io_timeout");
        }

        s.stop();
        t.join();
    }

    // ---- a reused connection is allowed to be quiet ----------------------
    {
        http::server::options o;

        o.initial_idle_timeout = 0.3;           // the new-connection bound
        o.idle_timeout = 3;                     // the between-requests bound

        http::server s(http::server::async_t(), 0, "127.0.0.1",
                       sys::tls_context(), sys::server::policy(), o);

        furnish(s);
        s.transport().on_error([](const std::exception&, const sys::peer&) {});

        std::thread t([&s]{ s.run(); });

        {
            sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

            c << "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

            util::http::Response first;

            ok("  the first request is answered", reply(c, first) &&
               first.status() == 200, first.body());

            // **The polling case.**  Longer than initial_idle_timeout and
            // shorter than idle_timeout: a connection that has already been
            // used has earned the benefit of the doubt that a brand new one
            // has not.  A server applying the new-connection bound here would
            // have hung up 600ms ago.
            std::this_thread::sleep_for(std::chrono::milliseconds(900));

            c << "GET /second HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

            util::http::Response second;

            const bool got = reply(c, second);

            ok("  and a quiet gap longer than the new-connection bound is fine",
               got && second.body() == "second\n", second.body());
        }

        s.stop();
        t.join();
    }

    // ---- once a request starts, it is a request --------------------------
    {
        http::server::options o;

        o.idle_timeout = 0.4;                   // short, and not the one

        sys::server::policy p;

        p.io_timeout = 4;                       // the request bound

        http::server s(http::server::async_t(), 0, "127.0.0.1",
                       sys::tls_context(), p, o);

        furnish(s);
        s.transport().on_error([](const std::exception&, const sys::peer&) {});

        std::thread t([&s]{ s.run(); });

        {
            sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

            c << "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n" << std::flush;

            util::http::Response first;

            ok("  the first request is answered", reply(c, first) &&
               first.status() == 200, first.body());

            // **The assertion that made this three bounds instead of two.**
            // The second request takes about a second to arrive, which is well
            // inside io_timeout and well outside idle_timeout.  A server that
            // bounds a reused request by the idle number -- which is what one
            // timer per request amounts to -- drops this at 400ms, and a large
            // POST sent as a second request would be the real version of it.
            const std::string head = "GET /second HTTP/1.1\r\nHost: x\r\n\r\n";

            bool broke = false;

            for(std::size_t i = 0; i < head.size(); i++) {
                c << head[i] << std::flush;

                if(!c) { broke = true; break; }

                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }

            util::http::Response second;

            const bool got = !broke && reply(c, second);

            ok("  a slow second request gets io_timeout, not idle_timeout",
               got && second.body() == "second\n",
               broke ? "the connection died mid-request" : second.body());
        }

        s.stop();
        t.join();
    }
}

/**
 * stop() while a connection is parked on a deadline.
 *
 * **The race, and it is not hypothetical -- this is the segfault.**  A parked
 * connection has a deadline timer registered in the reactor.  stop() ends the
 * wait by requesting the connection's cancel token, and it does that on the
 * *calling* thread, so the coroutine unwinds there while the reactor thread is
 * still running a pass.  Anything in that unwind that touches the reactor is a
 * data race on containers it mutates with no lock.
 *
 * It cost a "double free or corruption" in the Ubuntu container, a long way
 * from the cause and with a backtrace entirely inside malloc.  It did not
 * reproduce on macOS in fifteen runs.
 *
 * **The test is that this finishes.**  There is no clever assertion available:
 * the failure is heap corruption, so the honest form is to drive the race many
 * times and let a crash be the failure.  Both parked states are covered,
 * because they arm different timers: nothing sent at all parks in the idle
 * wait, a partial head parks in the request read.
 */
static void stopping_while_a_connection_is_parked() {
    std::cout << "\nstop() while a connection is parked on a deadline:\n";

    int rounds = 0;

    for(int i = 0; i < 40; i++) {
        http::server s(http::server::async_t(), 0, "127.0.0.1");

        furnish(s);
        s.transport().on_error([](const std::exception&, const sys::peer&) {});

        std::thread t([&s]{ s.run(); });

        {
            sys::socketstream c("127.0.0.1", s.port(), -1, 5.0);

            // Even rounds say nothing and park in the idle wait; odd rounds
            // send half a head and park in the request read.  Different
            // timers, same unwind.
            if(i % 2) c << "GET /hello HTTP/1.1\r\n" << std::flush;

            // Long enough for the accept and the park, short enough that forty
            // rounds are quick.  Hitting the window is what matters, and forty
            // tries at it is the test.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

            s.stop();
        }

        t.join();

        ++rounds;
    }

    ok("  forty rounds of stop() against a parked connection", rounds == 40,
       std::to_string(rounds) + " completed");
}

int main() {
    std::cout << "net_http_keepalive_test\n";

    try {
        two_requests_on_one_connection();
        a_pipelined_pair();
        a_body_is_consumed_whole();
        the_client_can_still_say_close();
        a_framing_error_closes();
        the_bounds();
        three_bounds_three_questions();
        stopping_while_a_connection_is_parked();
        a_streaming_response_still_closes();
        the_polite_goodbye();
        what_it_costs_and_saves();
    }
    catch(std::exception& e) {
        std::cerr << "net_http_keepalive_test: " << e.what() << "\n";
        return 1;
    }

    if(failures) {
        std::cerr << "net_http_keepalive_test: " << failures << " failed\n";
        return 1;
    }

    std::cout << "net_http_keepalive_test: all good\n";

    return 0;
}
