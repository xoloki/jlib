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
 * sys_async_tls_test -- a TLS connection driven by the reactor.
 *
 * The point is not that TLS works -- sys_tls_server_test covers the blocking
 * path -- but that the *two readiness questions* are both answered.  A reactor
 * reports a descriptor readable; it cannot report that an SSL is holding
 * decrypted plaintext nobody has taken.  A design that asked the descriptor
 * alone would hang waiting for octets that had already arrived, and the
 * section that catches that is the one where a whole message is delivered in a
 * single record and then read out in pieces.
 */

#include "certificate.hh"

#include <jlib/sys/async_tls.hh>
#include <jlib/sys/await.hh>
#include <jlib/sys/listener.hh>
#include <jlib/sys/reactor.hh>
#include <jlib/sys/sslstream.hh>
#include <jlib/sys/tls.hh>
#include <jlib/util/http.hh>

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

namespace sys = jlib::sys;
namespace http = jlib::util::http;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/**
 * A blocking jlib TLS server on a thread, so the async client has a real peer.
 *
 * Deliberately the *blocking* implementation: if the async one talked only to
 * itself, a shared misunderstanding of the protocol would pass.
 */
struct peer {
    sys::listener l;
    sys::tls_context ctx;
    std::thread t;

    peer(const std::string& cert, const std::string& key)
        : l(0, "127.0.0.1"), ctx(sys::tls_context::server(cert, key)) {}

    template<typename F>
    void serve(F f) {
        t = std::thread([this, f]{
            try {
                const int fd = l.accept(10);

                if(fd < 0) return;

                sys::tlsstream s(sys::tls_server, ctx, sys::adopt, fd, "", 0, 10);

                f(s);
            }
            catch(std::exception&) {}
        });
    }

    ~peer() { if(t.joinable()) t.join(); }
};

/**
 * The coroutines are file-scope functions, not lambdas, and that is not style.
 *
 * A lambda's captures live in the *lambda object*, not in the coroutine frame.
 * Written as `[&x](...) -> task<T> { ... }(arg)` the lambda is a temporary that
 * dies at the end of the call expression, and the coroutine then reads a
 * capture that is gone -- a use-after-free that shows up as a segfault
 * somewhere later and says nothing about where it came from.  This test had
 * exactly that bug.
 *
 * A function's *parameters* are copied into the frame, so they are safe.
 */

static sys::task<std::string> exchange_line(sys::async_tls& t) {
    co_await t.handshake();

    co_await t.writer().write("hello\r\n");

    std::string line;

    for(;;) {
        const int c = t.reader().get();

        if(c == sys::async_reader::empty) {
            if(!co_await t.reader().fill()) break;

            continue;
        }

        if(c == '\n') break;

        if(c != '\r') line += char(c);
    }

    co_return line;
}

static sys::task<void> serve_whole(sys::async_tls& t, std::string whole) {
    co_await t.handshake();

    // One call, one record.
    co_await t.writer().write(whole);
}

static sys::task<std::string> fetch_body(sys::async_tls& t, std::size_t cap) {
    co_await t.handshake();

    const std::string head = co_await http::read_head(t.reader(), 8192);
    const http::Response h = http::parse_head(head, false);

    co_return co_await http::read_body(t.reader(), h, cap);
}

static sys::task<bool> just_handshake(sys::async_tls& t) {
    co_await t.handshake();

    co_return true;
}

/** Connect, handshake, exchange a line. */
static void a_client_talks_to_a_blocking_server(const std::string& cert,
                                                const std::string& key)
{
    std::cout << "\nan async client talks to a blocking server:\n";

    peer p(cert, key);

    p.serve([](sys::tlsstream& s) {
        std::string line;
        std::getline(s, line);

        if(!line.empty() && line.back() == '\r') line.pop_back();

        s << "you said: " << line << "\r\n" << std::flush;
        s.close();
    });

    sys::reactor r;

    // A plain connected socket, not a tlsstream: async_tls takes it from here.
    sys::socketstream sock("localhost", p.l.port());

    sys::async_tls tls(r, sock.get_socket(), "localhost");

    ok("  it is not established before the handshake", !tls.established());

    sys::task<std::string> exchange = exchange_line(tls);

    const std::string got = sys::run_until_complete(r, exchange);

    ok("  the handshake completed", tls.established());

    ok("  and the exchange went through", got == "you said: hello", got);
}

/**
 * The section the design exists for.
 *
 * The server sends a whole message in one write -- so it arrives as one TLS
 * record, decrypted in one go -- and the client then reads it out a little at
 * a time.  After the first read the socket has nothing left on it and the SSL
 * has everything.  A reader that waited on the descriptor would wait forever.
 */
static void plaintext_the_reactor_cannot_see(const std::string& cert,
                                             const std::string& key)
{
    std::cout << "\nbuffered plaintext the reactor cannot see:\n";

    // **The server is async here too, and that is not laziness.**  The
    // condition being staged needs the whole payload in *one* TLS record, so
    // that one SSL_read drains the socket and leaves the plaintext inside the
    // SSL.  The blocking tlsstream cannot produce it: its put area is 1024
    // octets, so a large write becomes a run of SSL_writes and a run of
    // records, the socket stays readable between them, and a
    // descriptor-only reader never notices it was wrong.
    //
    // An earlier version of this test used the blocking server and passed
    // against a deliberately naive reader.  That is the whole reason this one
    // looks the way it does.

    sys::listener l(0, "127.0.0.1");
    sys::tls_context ctx = sys::tls_context::server(cert, key);

    const std::string payload(8192, 'x');

    const std::string whole =
        "HTTP/1.1 200 OK\r\nContent-Length: " +
        std::to_string(payload.size()) + "\r\n\r\n" + payload;

    std::thread server([&l, &ctx, whole]{
        try {
            const int fd = l.accept(10);

            if(fd < 0) return;

            sys::reactor sr;
            sys::async_tls s(sr, fd, sys::tls_server, ctx);

            sys::task<void> serve = serve_whole(s, whole);

            sys::run_until_complete(sr, serve);

            // Held open, so nothing the client does can be satisfied by a
            // close -- which is the other thing that would wake a
            // descriptor-only reader.
            std::this_thread::sleep_for(std::chrono::milliseconds(400));

            ::close(fd);
        }
        catch(std::exception&) {}
    });

    sys::reactor r;
    sys::socketstream sock("localhost", l.port());
    sys::async_tls tls(r, sock.get_socket(), "localhost");

    sys::task<std::string> read_it = fetch_body(tls, 65536);

    // Driven by hand rather than with run_until_complete, and bounded.  A
    // reader that got this wrong does not return a wrong answer -- it *hangs*,
    // waiting on a descriptor that will never be readable again, and a test
    // whose failure mode is a hung suite is a bad test.  Two seconds is far
    // beyond the six milliseconds the right answer takes and comfortably short
    // of the four hundred the server holds the connection for.
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(2);

    read_it.start();

    while(!read_it.done() && std::chrono::steady_clock::now() < deadline)
        r.run_one(std::chrono::milliseconds(50));

    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
        .count();

    // The assertion the section exists for.  A descriptor-only reader is still
    // parked here, because the socket was drained into the SSL and the reactor
    // has nothing to report.
    // Two assertions, and only the second one catches the design error.  A
    // reader that asks the descriptor first still *finishes* -- the server's
    // close four hundred milliseconds later wakes it -- so "did it finish"
    // passes either way.  What separates them is how long it took: measured,
    // 8.8ms against 406ms.
    ok("  the read finished at all", read_it.done(),
       read_it.done() ? std::to_string(took) + "s"
                      : "still parked after " + std::to_string(took) + "s");

    std::string body;

    if(read_it.done()) {
        try { body = read_it.result(); }
        catch(std::exception& e) { ok("  it did not throw", false, e.what()); }
    }

    server.join();

    ok("  and the body came out of the SSL's own buffer", body == payload,
       std::to_string(body.size()) + " octets");

    ok("  and without ever waiting on the descriptor", took < 0.35,
       std::to_string(took) + "s");
}

/** A framing function converted for plain descriptors works over TLS unchanged. */
static void the_framing_functions_do_not_know(const std::string& cert,
                                              const std::string& key)
{
    std::cout << "\nthe framing functions do not know which they have:\n";

    peer p(cert, key);

    p.serve([](sys::tlsstream& s) {
        s << "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
          << "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n" << std::flush;

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        s.close();
    });

    sys::reactor r;
    sys::socketstream sock("localhost", p.l.port());
    sys::async_tls tls(r, sock.get_socket(), "localhost");

    sys::task<std::string> get = fetch_body(tls, 8192);

    const std::string body = sys::run_until_complete(r, get);

    // read_body was written and tested against a pipe, and was not touched
    // when the TLS reader arrived.
    ok("  a chunked body reads the same over TLS", body == "hello world",
       "\"" + body + "\"");
}

/** The certificate still has to belong to the host asked for. */
static void the_name_is_still_checked(const std::string& cert,
                                      const std::string& key)
{
    std::cout << "\nthe name is still checked:\n";

    peer p(cert, key);

    p.serve([](sys::tlsstream&) {});

    sys::reactor r;
    sys::socketstream sock("127.0.0.1", p.l.port());

    // A certificate for "localhost", asked to be good for something else.
    sys::async_tls tls(r, sock.get_socket(), "not.the.name");

    sys::task<bool> shake = just_handshake(tls);

    bool threw = false;
    std::string why;

    try { sys::run_until_complete(r, shake); }
    catch(std::exception& e) { threw = true; why = e.what(); }

    ok("  a certificate for the wrong host is refused", threw, why);

    ok("  and it is not established", !tls.established());
}

/**
 * A handshake against a peer that accepts and then says nothing.
 *
 * The shape a slow-loris takes on a TLS port: the connection completes, the
 * ClientHello goes out, and the server never answers.  SSL_connect returns
 * WANT_READ and the reader parks on a descriptor that will never be ready.
 *
 * Named in the previous commit as untested; this is it.
 */
static void a_handshake_can_be_cancelled() {
    std::cout << "\na handshake can be cancelled:\n";

    sys::listener l(0, "127.0.0.1");

    // Accepts and holds the connection open without speaking TLS at all.
    std::thread mute([&l]{
        try {
            const int fd = l.accept(10);

            if(fd < 0) return;

            std::this_thread::sleep_for(std::chrono::seconds(3));

            ::close(fd);
        }
        catch(std::exception&) {}
    });

    sys::reactor r;
    sys::socketstream sock("127.0.0.1", l.port());

    sys::cancel_token t = sys::cancel_token::create();

    sys::async_tls tls(r, sock.get_socket(), "localhost", t);

    sys::task<bool> shake = just_handshake(tls);

    shake.start();

    ok("  the handshake is parked", !shake.done());

    sys::deadline(r, std::chrono::milliseconds(100), t);

    const auto start = std::chrono::steady_clock::now();

    bool cancelled = false;

    try { sys::run_until_complete(r, shake); }
    catch(sys::cancelled&) { cancelled = true; }

    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
        .count();

    ok("  a deadline ends it", cancelled);

    // Without this it would sit until the peer closed at three seconds, or
    // forever against one that did not.
    ok("  well before the peer gives up", took < 1.5,
       std::to_string(took) + "s");

    ok("  and it never established", !tls.established());

    mute.join();
}

int main() {
    std::cout << std::unitbuf;

    const std::string cert = "async_tls_cert.pem";
    const std::string key = "async_tls_key.pem";

    if(!make_cert(cert, key)) {
        std::cerr << "could not generate a test certificate, skipping"
                  << std::endl;

        return 77;
    }

    ::setenv("SSL_CERT_FILE", cert.c_str(), 1);

    a_client_talks_to_a_blocking_server(cert, key);
    plaintext_the_reactor_cannot_see(cert, key);
    the_framing_functions_do_not_know(cert, key);
    the_name_is_still_checked(cert, key);
    a_handshake_can_be_cancelled();

    std::remove(cert.c_str());
    std::remove(key.c_str());

    // What a green run does not establish.
    //
    // That the *server* side handshakes asynchronously.  async_tls takes a
    // tls_server_t and the code path exists, but every test here is an async
    // client against a blocking jlib server -- which is deliberate, since two
    // async ends talking only to each other would pass a shared
    // misunderstanding.  Nothing has driven SSL_accept through the reactor.
    //
    // Not a real peer.  Both ends are jlib and the certificate is one this
    // test generated, as sys_tls_server_test says of itself.
    //
    // Not renegotiation or a KeyUpdate, which is the case WANT_WRITE on a read
    // exists for.  The branch is there and is reached by nothing here; TLS 1.3
    // makes it rare and OpenSSL handles most of it internally.
    //
    // Nothing about the descriptor being shared.  async_tls sets O_NONBLOCK and
    // restores it on destruction, and the rule that a descriptor is either
    // async or blocking is documented rather than enforced -- a caller that
    // hands the same one to a socketstream at the same time gets EAGAIN, and
    // nothing here stops them.
    //
    // And nothing about cancelling a handshake, which the token reaches
    // through want() but which is not exercised.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
