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
 * sys_async_server_test -- a server whose connections do not occupy threads.
 *
 * sys_server_test covers the blocking server and still passes unaltered.  What
 * is new is that a connection lives on the reactor as a suspended coroutine
 * rather than on a pool thread, so the assertions worth making are about how
 * many can be in flight at once, what bounds them, and what happens to one
 * that is parked when the server stops.
 */

#include "certificate.hh"

#include <jlib/sys/server.hh>
#include <jlib/sys/socketstream.hh>
#include <jlib/sys/sslstream.hh>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace sys = jlib::sys;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Read one line from a connection.  @return false at end of stream. */
static sys::task<bool> line_from(sys::server::connection& c, std::string& out) {
    out.clear();

    for(;;) {
        const int ch = c.reader().get();

        if(ch == sys::async_reader::empty) {
            if(!co_await c.reader().fill()) co_return !out.empty();

            continue;
        }

        if(ch == '\n') {
            if(!out.empty() && out.back() == '\r') out.pop_back();

            co_return true;
        }

        out += static_cast<char>(ch);
    }
}

/** Read a line, echo it back. */
static sys::task<void> echo(sys::server::connection& c, const sys::peer&) {
    std::string line;

    if(!co_await line_from(c, line)) co_return;

    co_await c.writer().write("echo: " + line + "\r\n");
}

/** One exchange from a blocking client, which is what a real one is. */
static std::string say(unsigned short port, const std::string& what) {
    sys::socketstream s("127.0.0.1", port, -1, 5.0);

    s << what << "\r\n" << std::flush;

    std::string line;

    std::getline(s, line);

    if(!line.empty() && line.back() == '\r') line.pop_back();

    return line;
}

static void it_serves_a_connection() {
    std::cout << "\nit serves a connection:\n";

    sys::server s(0, sys::server::async_handler(echo), "127.0.0.1");

    std::thread t([&s]{ s.run(); });

    ok("  the exchange goes through", say(s.port(), "hello") == "echo: hello");

    ok("  and another, on a new connection",
       say(s.port(), "again") == "echo: again");

    s.stop();
    t.join();
}

// The concurrency section needs state the handler can reach, and a handler is
// a plain function pointer's worth of thing -- so the state is here.
namespace {
    std::atomic<int> g_live(0);
    std::atomic<int> g_most(0);
    std::atomic<bool> g_release(false);
}

/**
 * Arrive, park, and answer only when released.
 *
 * So every connection is in flight at the same time, and g_most records how
 * many that was.
 */
static sys::task<void> hold(sys::server::connection& c, const sys::peer&) {
    std::string line;

    if(!co_await line_from(c, line)) co_return;

    const int now = ++g_live;

    int seen = g_most.load();

    while(now > seen && !g_most.compare_exchange_weak(seen, now)) ;

    // Parked on the reactor, not on a thread.  A blocking server with no pool
    // would be serving exactly one connection here.
    while(!g_release.load()) {
        co_await sys::sleep_for(c.reactor(), std::chrono::milliseconds(5));
    }

    co_await c.writer().write("released\r\n");

    --g_live;
}

/**
 * Many at once, with no pool at all.
 *
 * policy::threads is zero, so a *blocking* server would serve one connection
 * at a time -- sys_server_test asserts exactly that. Here every one of them is
 * suspended inside the same reactor on the same thread.
 */
static void many_at_once_with_no_pool() {
    std::cout << "\nmany at once, with no pool:\n";

    g_live = 0;
    g_most = 0;
    g_release = false;

    sys::server::policy p;

    p.threads = 0;
    p.max_connections = 64;

    sys::server s(0, sys::server::async_handler(hold), "127.0.0.1",
                  sys::tls_context(), p);

    std::thread t([&s]{ s.run(); });

    const int N = 8;

    std::vector<std::thread> clients;
    std::atomic<int> answered(0);

    for(int i = 0; i < N; i++) {
        clients.push_back(std::thread([&s, &answered]{
            try {
                if(say(s.port(), "waiting") == "released") ++answered;
            }
            catch(std::exception&) {}
        }));
    }

    // Wait for them all to be in flight, then let them go.
    const auto start = std::chrono::steady_clock::now();

    while(g_live.load() < N &&
          std::chrono::duration<double>(std::chrono::steady_clock::now()
                                        - start).count() < 5.0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ok("  all of them are in flight at once", g_most.load() == N,
       std::to_string(g_most.load()) + " of " + std::to_string(N));

    g_release = true;

    for(std::size_t i = 0; i < clients.size(); i++) clients[i].join();

    ok("  and every one was answered", answered.load() == N,
       std::to_string(answered.load()) + " of " + std::to_string(N));

    s.stop();
    t.join();
}

/**
 * A connection parked on a client that never speaks.
 *
 * stop() has to end it.  Before #212 a token could only be *set*, and a
 * coroutine suspended on a descriptor that never became ready would never
 * notice -- so the server would wait on shutdown for a peer that had no
 * intention of speaking.
 */
static void stopping_ends_a_parked_connection() {
    std::cout << "\nstopping ends a connection parked on a silent client:\n";

    sys::server s(0, sys::server::async_handler(echo), "127.0.0.1");

    std::thread t([&s]{ s.run(); });

    // Connects and says nothing at all.  The handler is now parked in
    // reader().fill().
    sys::socketstream quiet("127.0.0.1", s.port(), -1, 5.0);

    const auto start = std::chrono::steady_clock::now();

    // Give the accept and the first fill() time to happen.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    s.stop();
    t.join();

    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
        .count();

    // Without cancellation that ends a wait, this hangs until the client goes
    // away -- and the client is this thread.
    ok("  stop() returns promptly", took < 3.0, std::to_string(took) + "s");
}

/** A handler can bound a silent client itself, which is what #166 is about. */
static sys::task<void> with_deadline(sys::server::connection& c,
                                     const sys::peer&) {
    sys::deadline(c.reactor(), std::chrono::milliseconds(120), c.token());

    std::string line;

    try {
        if(!co_await line_from(c, line)) co_return;
    }
    catch(sys::cancelled&) {
        // The token is spent, so this write cannot use it -- but the
        // descriptor is still open and the client is still there.
        co_return;
    }

    co_await c.writer().write("echo: " + line + "\r\n");
}

static void a_handler_can_set_its_own_deadline() {
    std::cout << "\na handler can set its own deadline:\n";

    sys::server s(0, sys::server::async_handler(with_deadline), "127.0.0.1");

    std::thread t([&s]{ s.run(); });

    {
        sys::socketstream quiet("127.0.0.1", s.port(), -1, 5.0);

        const auto start = std::chrono::steady_clock::now();

        // The server closes when the handler returns, so the read ends.
        std::string ignored;

        std::getline(quiet, ignored);

        const double took =
            std::chrono::duration<double>(std::chrono::steady_clock::now()
                                          - start).count();

        ok("  a silent client is dropped at the deadline",
           took >= 0.10 && took < 3.0, std::to_string(took) + "s");
    }

    // And one that speaks in time is served normally.
    ok("  while one that speaks is served",
       say(s.port(), "quick") == "echo: quick");

    s.stop();
    t.join();
}

/**
 * TLS on the async path.
 *
 * server::serve_async has had the branch since it was written and nothing had
 * ever run it: sys_async_tls_test drives async_tls from the *client* side, so
 * SSL_accept through the reactor was code with no test -- which is true until
 * somebody runs it.
 *
 * The client is the blocking tlsstream, deliberately.  Two async ends talking
 * only to each other would pass a shared misunderstanding.
 */
static void it_serves_a_tls_connection(const std::string& cert,
                                       const std::string& key)
{
    std::cout << "\nit serves a TLS connection:\n";

    sys::tls_context ctx = sys::tls_context::server(cert, key);

    sys::server s(0, sys::server::async_handler(echo), "127.0.0.1", ctx);

    std::thread t([&s]{ s.run(); });

    std::string got;
    std::string threw;

    try {
        sys::tlsstream c("localhost", s.port(), false, 10.0, 10.0);

        c << "over tls\r\n" << std::flush;

        std::getline(c, got);

        if(!got.empty() && got.back() == '\r') got.pop_back();
    }
    catch(std::exception& e) { threw = e.what(); }

    ok("  the handshake was answered through the reactor", threw.empty(),
       threw);

    ok("  and the exchange went through", got == "echo: over tls", got);

    // A second, so the context is proved reusable across connections on this
    // path as sys_tls_server_test proves it on the blocking one.
    try {
        sys::tlsstream c("localhost", s.port(), false, 10.0, 10.0);

        c << "again\r\n" << std::flush;

        std::string second;

        std::getline(c, second);

        if(!second.empty() && second.back() == '\r') second.pop_back();

        ok("  and so did a second connection", second == "echo: again",
           second);
    }
    catch(std::exception& e) { ok("  and so did a second connection", false,
                                  e.what()); }

    s.stop();
    t.join();
}

/**
 * The connection cap, and the two lines that enforce it.
 *
 * Reaching it disables the listener's registration -- modify(m_listen, NONE)
 * -- so the overflow waits in the kernel's listen backlog rather than being
 * accepted and refused.  reap() re-arms it when a slot frees.  Neither line
 * had ever run: the previous test used eight connections against a cap of
 * sixty-four.
 */
static void the_connection_cap_holds() {
    std::cout << "\nthe connection cap holds:\n";

    g_live = 0;
    g_most = 0;
    g_release = false;

    sys::server::policy p;

    p.threads = 0;
    p.max_connections = 2;

    sys::server s(0, sys::server::async_handler(hold), "127.0.0.1",
                  sys::tls_context(), p);

    std::thread t([&s]{ s.run(); });

    const int N = 6;

    std::vector<std::thread> clients;
    std::atomic<int> answered(0);

    for(int i = 0; i < N; i++) {
        clients.push_back(std::thread([&s, &answered]{
            try {
                if(say(s.port(), "waiting") == "released") ++answered;
            }
            catch(std::exception&) {}
        }));
    }

    // Long enough that every client has connected and any that were going to
    // be accepted have been.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    ok("  no more than the cap are in flight", g_most.load() <= 2,
       std::to_string(g_most.load()) + " of a cap of 2");

    ok("  and the cap was actually reached", g_most.load() == 2,
       std::to_string(g_most.load()));

    // Letting them go frees slots, which reap() notices and which re-arms the
    // listener -- so the four still in the backlog are accepted.
    g_release = true;

    for(std::size_t i = 0; i < clients.size(); i++) clients[i].join();

    ok("  every connection is served once slots come free",
       answered.load() == N,
       std::to_string(answered.load()) + " of " + std::to_string(N));

    s.stop();
    t.join();
}

int main() {
    std::cout << std::unitbuf;

    const std::string cert = "async_server_cert.pem";
    const std::string key = "async_server_key.pem";

    const bool have_cert = make_cert(cert, key);

    if(have_cert) ::setenv("SSL_CERT_FILE", cert.c_str(), 1);

    it_serves_a_connection();
    many_at_once_with_no_pool();
    the_connection_cap_holds();

    if(have_cert) it_serves_a_tls_connection(cert, key);
    else std::cout << "\n  skip  no certificate, so the TLS path is not run\n";
    stopping_ends_a_parked_connection();
    a_handler_can_set_its_own_deadline();

    // What a green run does not establish.
    //
    // Not a real peer.  Every client here is jlib, and the TLS one uses a
    // certificate this test generated, as sys_tls_server_test says of itself.
    //
    // Not a client that goes away mid-handshake, which is the case
    // serve_async's TLS branch would have to unwind from.  The handshake is
    // awaited and nothing here abandons one.
    //
    // And nothing about what a *blocking* server would have done differently
    // here beyond the note in the concurrency section.  The claim that eight
    // connections could not be in flight at once with threads == 0 rests on
    // sys_server_test's own assertion, not on anything measured here.
    if(have_cert) { std::remove(cert.c_str()); std::remove(key.c_str()); }

    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
