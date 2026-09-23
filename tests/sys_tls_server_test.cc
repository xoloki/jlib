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

// The accepting end of a TLS connection, which jlib could not be until now.
//
// basic_tlsbuf called SSL_connect unconditionally and there was no way to give
// an SSL_CTX a certificate and a key, so two tests hand-rolled SSL_CTX_new,
// SSL_new and SSL_accept around jlib's own listener -- and one of them wrote
// down that it was working around a gap rather than fixing one.  This is the
// gap closed, tested before anything is built on top of it: sys::listener and
// sys::tlsstream, no sys::server yet.
//
// Entirely local, and that is the point.  The certificate is generated at
// runtime, SSL_CERT_FILE points the client's trust store at it, and both ends
// are in this process -- so server-side TLS is provable on a developer's
// machine and not only in the build container, which is exactly what the old
// arrangement could not do.
//
// The verification is real.  Nothing here is turned off; the handshake
// succeeds because the certificate is genuinely trusted for this run.

#include "certificate.hh"

#include <jlib/sys/listener.hh>
#include <jlib/sys/socketstream.hh>
#include <jlib/sys/sslstream.hh>
#include <jlib/sys/tls.hh>

#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace sys = jlib::sys;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Serve one TLS connection: read a line, echo it back, close. */
static void echo_once(sys::listener& l, const sys::tls_context& ctx,
                      std::string* saw = 0, bool* handshook = 0)
{
    try {
        const int fd = l.accept(10);

        if(fd < 0) return;

        sys::tlsstream s(sys::tls_server, ctx, sys::adopt, fd, "", 0, 10);

        if(handshook) *handshook = true;

        std::string line;
        std::getline(s, line);

        if(saw) *saw = line;

        s << "you said: " << line << "\r\n" << std::flush;
        s.close();
    }
    catch(std::exception& e) {
        if(saw) *saw = std::string("server threw: ") + e.what();
    }
}

/**
 * A session ticket outlives one key rotation and not two.
 *
 * **What the window is, rather than what it happens to be.** OpenSSL issues
 * tickets under a key it generates when the context is built and never
 * changes, so before this the forward-secrecy window was however long the
 * process kept that context -- for the deployed jhttpd, months. The key now
 * rotates on a timer derived from the session timeout, and the previous key
 * is kept so that rotating does not throw away resumptions still inside it.
 *
 * Three states, and the third is the one worth having:
 *
 *   - a ticket resumes at all, which is the baseline everything else needs
 *   - after one rotation it still resumes, from the retained key, so clients
 *     do not pay a full handshake every time the key turns over
 *   - after two it does not, because the key that issued it is gone -- which
 *     is the bound, and the only assertion here that would notice if the
 *     rotation quietly stopped happening
 *
 * The client is raw OpenSSL because resumption needs an SSL_SESSION and
 * jlib's streams do not expose one. What is under test is the server's
 * ticket callback, and the server here is jlib's.
 */
static SSL_SESSION* g_ticket = 0;

static int keep_the_ticket(SSL*, SSL_SESSION* s)
{
    // TLS 1.3 sends NewSessionTicket *after* the handshake, so the session
    // worth resuming is the one that arrives here rather than whatever
    // SSL_get1_session would hand back when SSL_connect returns.
    if(g_ticket) SSL_SESSION_free(g_ticket);

    g_ticket = s;

    return 1;   // we hold the reference now
}

/** A plain connected descriptor, which socketstream deliberately does not lend out. */
static int dial(unsigned short port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if(fd < 0) return -1;

    sockaddr_in at;

    std::memset(&at, 0, sizeof at);

    at.sin_family = AF_INET;
    at.sin_port = htons(port);
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if(::connect(fd, reinterpret_cast<sockaddr*>(&at), sizeof at) != 0) {
        ::close(fd);

        return -1;
    }

    return fd;
}

/** One connection; true if the server resumed rather than handshook afresh. */
static bool talk(SSL_CTX* cc, unsigned short port, bool offer_ticket)
{
    const int fd = dial(port);

    if(fd < 0) return false;

    SSL* ssl = SSL_new(cc);

    SSL_set_fd(ssl, fd);

    if(offer_ticket && g_ticket) SSL_set_session(ssl, g_ticket);

    bool reused = false;

    if(SSL_connect(ssl) == 1) {
        reused = SSL_session_reused(ssl) == 1;

        // Say something and read the answer: on TLS 1.3 the ticket rides in
        // after the handshake, and a client that hangs up immediately never
        // sees it.
        SSL_write(ssl, "hello\r\n", 7);

        char buf[128];

        SSL_read(ssl, buf, sizeof buf);

        SSL_shutdown(ssl);
    }

    SSL_free(ssl);
    ::close(fd);

    return reused;
}

static void a_ticket_outlives_one_rotation_and_not_two(const std::string& cert,
                                                       const std::string& key)
{
    std::cout << "\nsession tickets, and the window they leave open:\n";

    sys::tls_context ctx = sys::tls_context::server(cert, key);
    sys::listener l(0, "127.0.0.1");

    const unsigned short port = l.port();

    // Six exchanges: three pairs of "get a ticket, then try to use it".
    std::thread server([&l, &ctx] {
        for(int i = 0; i < 6; i++) echo_once(l, ctx);
    });

    SSL_CTX* cc = SSL_CTX_new(TLS_client_method());

    SSL_CTX_set_session_cache_mode(cc, SSL_SESS_CACHE_CLIENT |
                                       SSL_SESS_CACHE_NO_INTERNAL_STORE);
    SSL_CTX_sess_set_new_cb(cc, keep_the_ticket);
    SSL_CTX_set_verify(cc, SSL_VERIFY_NONE, 0);

    // **A ticket is spent by the connection that uses it.** TLS 1.3 tickets
    // are single-use on OpenSSL's client, so every state below gets its own
    // full handshake to draw a fresh one from. The first version of this test
    // offered the same session three times and read the client's refusal to
    // reuse it as the server refusing to resume -- which reported the bug
    // that this test exists to detect, from a server that did not have it.
    const bool first = talk(cc, port, false);

    ok("a first connection is not a resumption", !first);
    ok("and it leaves a ticket behind", g_ticket != 0);

    ok("the ticket resumes the session", talk(cc, port, true));

    // One rotation: the key that issued this ticket becomes the previous
    // key, which is kept exactly so that this still works.
    talk(cc, port, false);

    ok("rotation is available on a server context", ctx.rotate_ticket_key());

    ok("a ticket from before one rotation still resumes",
       talk(cc, port, true));

    // Two rotations: the issuing key is gone and a full handshake is right.
    talk(cc, port, false);

    ctx.rotate_ticket_key();
    ctx.rotate_ticket_key();

    ok("and one from before two rotations does not", !talk(cc, port, true));

    server.join();

    if(g_ticket) { SSL_SESSION_free(g_ticket); g_ticket = 0; }

    SSL_CTX_free(cc);
}

/**
 * A server that is slow is not a server that finished.
 *
 * The distinction basic_socketbuf has had all along and basic_tlsbuf did not.
 * A read timeout arrives as eof() -- a streambuf has nothing else to say -- so
 * without timed_out() a caller reads a stall as a clean end of response, and
 * util::http's until_close framing hands back a truncated body as a complete
 * one.
 *
 * The last assertion is the whole point and could not be written before: after
 * the timeout the connection is *still there*, and clearing the stream reads
 * what the server eventually said.
 */
static void a_slow_server_is_not_a_finished_one(const std::string& cert,
                                                const std::string& key)
{
    std::cout << "\na slow server is not a finished one:\n";

    sys::tls_context ctx = sys::tls_context::server(cert, key);
    sys::listener l(0, "127.0.0.1");

    std::thread server([&l, &ctx]{
        try {
            const int fd = l.accept(10);

            if(fd < 0) return;

            sys::tlsstream s(sys::tls_server, ctx, sys::adopt, fd, "", 0, 10);

            // Long enough that the client's half-second bound expires first,
            // and the silence is under this test's control rather than the
            // network's.
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));

            s << "late\r\n" << std::flush;

            // Held open until the client has read it, so the second read
            // cannot be satisfied by a close rather than by the data.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            s.close();
        }
        catch(std::exception&) {}
    });

    // A generous handshake bound, then a tight one for the read: the
    // handshake must not be what times out.
    sys::tlsstream client("localhost", l.port(), false, 10.0, 10.0);

    client.set_timeout(0.5);

    const auto start = std::chrono::steady_clock::now();

    std::string line;

    std::getline(client, line);

    const double waited =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
        .count();

    // One-sided at the bottom, loose at the top: it must have waited for the
    // bound, and a loaded machine must not make it fail.
    ok("  the read gives up near the bound", waited >= 0.4 && waited < 3.0,
       std::to_string(waited) + "s");

    ok("  and says it timed out rather than ended", client.timed_out());

    ok("  though the stream itself can only say eof", client.eof());

    // The assertion this commit exists for.  Before it, timed_out() was
    // permanently false on a TLS stream and this was indistinguishable from a
    // server that had closed.
    //
    // The bound is raised first, because it is still half a second and the
    // server has not spoken yet -- reading again on the old one would expire
    // again, which is correct behaviour and not what is being tested.  This
    // is what a caller who consulted timed_out() would do: decide to wait
    // longer rather than conclude the response was complete.
    client.clear();
    client.set_timeout(10.0);

    std::string late;

    std::getline(client, late);

    if(!late.empty() && late.back() == '\r') late.pop_back();

    ok("  and the connection was still there all along", late == "late",
       "\"" + late + "\"");

    server.join();
}

/**
 * STARTTLS refuses to upgrade a stream with bytes already in it.
 *
 * CVE-2011-0411.  underflow() is only called when the get area is empty, so
 * plaintext buffered before the handshake is served from the buffer after it
 * -- delivered to the caller as though it had arrived over TLS.
 *
 * No TLS is needed to show it: the guard is in start(), before open_ssl(), so
 * a socketpair and a delayed buffer are the whole fixture.  What is being
 * asserted is that start() **refuses**, not that the handshake fails.
 */
static void starttls_refuses_a_stream_with_bytes_in_it() {
    std::cout << "\nSTARTTLS refuses a stream with bytes in it:\n";

    int sv[2];

    if(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::cout << "  skip  socketpair failed\n";

        return;
    }

    // The server's legitimate answer and an injected line, in one write --
    // which is what an attacker who can write to the socket does, and what
    // one read(2) then collects together.
    const std::string both =
        "a001 OK Begin TLS negotiation now\r\n"
        "* CAPABILITY IMAP4rev1 AUTH=PLAIN\r\n";

    if(::write(sv[1], both.data(), both.size()) !=
       ssize_t(both.size())) {
        std::cout << "  skip  the peer write was short\n";

        ::close(sv[0]);
        ::close(sv[1]);

        return;
    }

    // delay = true: connected, not yet handshaken, which is the STARTTLS
    // state.  No certificate and no context are involved.
    sys::basic_sslbuf<char> buf("localhost", true, sys::adopt, sv[0],
                                "localhost", 0, 10);

    std::istream is(&buf);

    std::string line;

    std::getline(is, line);

    // The honesty guard: TCP does not promise the two lines arrive in one
    // read, and if they did not there is nothing buffered and nothing to
    // assert.  On a socketpair with one write they do, but say so rather than
    // depend on it.
    if(buf.in_avail() <= 0) {
        std::cout << "  skip  the two lines did not arrive together\n";

        ::close(sv[1]);

        return;
    }

    ok("  the injected line is sitting in the buffer",
       buf.in_avail() > 0, std::to_string(buf.in_avail()) + " octets");

    bool threw = false;
    std::string why;

    try { buf.start(); }
    catch(std::exception& e) { threw = true; why = e.what(); }

    // **Weak on its own, and deliberately kept anyway.**  Without the guard
    // this still passes: start() reaches open_ssl(), the socketpair has no
    // TLS peer, and SSL_connect throws.  Verified by reverting the guard --
    // this line went green and only the next one went red.
    ok("  and start() refuses to hand it over as authenticated", threw);

    // **This is the assertion that tests the fix.**  Matched on text, because
    // "it threw" is what the vulnerable code does here too, for an unrelated
    // reason.  Against a real server the handshake would *succeed* and the
    // buffered line would then be answered as authenticated, which is the
    // whole bug and which no socketpair can stage.
    ok("  saying what it found, not just that it failed",
       why.find("already buffered") != std::string::npos, why);

    ::close(sv[1]);
}

/**
 * And the unflushed half of the same guard.
 *
 * Plaintext written and not flushed would go out encrypted to a peer that is
 * not expecting it.  Nothing in the tree produces this -- command() flushes --
 * which is exactly why it is worth pinning.
 */
static void starttls_refuses_a_stream_with_bytes_pending() {
    std::cout << "\nSTARTTLS refuses a stream with bytes pending:\n";

    int sv[2];

    if(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        std::cout << "  skip  socketpair failed\n";

        return;
    }

    sys::basic_sslbuf<char> buf("localhost", true, sys::adopt, sv[0],
                                "localhost", 0, 10);

    std::ostream os(&buf);

    os << "STARTTLS";          // no flush

    bool threw = false;
    std::string why;

    try { buf.start(); }
    catch(std::exception& e) { threw = true; why = e.what(); }

    ok("  it refuses", threw);

    ok("  and says the bytes would have gone out encrypted",
       why.find("not flushed") != std::string::npos, why);

    ::close(sv[1]);
}

/**
 * And a legitimate STARTTLS still works, on both ends.
 *
 * A guard that refuses the attack and the feature alike is worse than the
 * bug, so this is the half that matters just as much. It also closes a gap
 * this file already wrote down by hand: STARTTLS-as-a-server was not
 * exercised anywhere.
 */
static void a_legitimate_starttls_still_works(const std::string& cert,
                                              const std::string& key)
{
    std::cout << "\na legitimate STARTTLS still works:\n";

    sys::tls_context ctx = sys::tls_context::server(cert, key);
    sys::listener l(0, "127.0.0.1");

    std::string saw_command;
    std::string saw_secret;

    std::thread server([&l, &ctx, &saw_command, &saw_secret]{
        try {
            const int fd = l.accept(10);

            if(fd < 0) return;

            // delay = true: accepted in the clear, handshake deferred.
            sys::tlsstream s(sys::tls_server, ctx, sys::adopt, fd, "", 0, 10,
                             true);

            std::getline(s, saw_command);

            if(!saw_command.empty() && saw_command.back() == '\r')
                saw_command.pop_back();

            s << "a001 OK begin TLS\r\n" << std::flush;

            s.start();

            std::getline(s, saw_secret);

            if(!saw_secret.empty() && saw_secret.back() == '\r')
                saw_secret.pop_back();

            s << "a002 OK\r\n" << std::flush;
            s.close();
        }
        catch(std::exception& e) {
            saw_command = std::string("server threw: ") + e.what();
        }
    });

    sys::tlsstream client("localhost", l.port(), true, 10.0, 10.0);

    client << "a001 STARTTLS\r\n" << std::flush;

    std::string answer;

    std::getline(client, answer);

    if(!answer.empty() && answer.back() == '\r') answer.pop_back();

    ok("  the server answers in the clear", answer == "a001 OK begin TLS",
       answer);

    // Nothing buffered either side, so the guard has nothing to object to --
    // which is the case it must not break.
    client.start();

    client << "a002 LOGIN secret\r\n" << std::flush;

    std::string done;

    std::getline(client, done);

    if(!done.empty() && done.back() == '\r') done.pop_back();

    ok("  and the handshake goes through afterwards", done == "a002 OK", done);

    server.join();

    ok("  the server read the command in the clear",
       saw_command == "a001 STARTTLS", saw_command);

    ok("  and the rest over TLS", saw_secret == "a002 LOGIN secret",
       saw_secret);
}

static void a_context_reads_a_certificate(const std::string& cert,
                                          const std::string& key)
{
    std::cout << "\na context reads a certificate:\n";

    bool made = false;

    try {
        sys::tls_context ctx = sys::tls_context::server(cert, key);

        made = !ctx.empty() && ctx.get() != 0;
    }
    catch(std::exception& e) {
        ok("a server context builds", false, e.what());
    }

    ok("a server context builds", made);

    ok("an empty one knows it is empty", sys::tls_context().empty() &&
       !static_cast<bool>(sys::tls_context()));

    // A refcount, which is the whole reason this type exists: basic_tlsbuf
    // built one SSL_CTX per connection and freed it in close(), and a server
    // reading its certificate and key once per connection is both slow and
    // wrong.
    {
        sys::tls_context a = sys::tls_context::server(cert, key);
        sys::tls_context b = a;

        ok("copying one shares the context", a.get() == b.get() && a.get() != 0);
    }

    // Now rather than on the first client to connect, where it reads as a
    // handshake failure and the certificate is the last thing anyone looks at.
    bool threw = false;
    std::string message;

    // **Named outside the try, and removed after it.**  These used to be
    // removed on the last two lines *inside* the try -- after the call that is
    // supposed to throw, which is the whole assertion -- so the cleanup ran
    // only when the case could not be set up and never when the test passed.
    // Two stray .pem files were left in the build directory every green run,
    // which `make check` cannot see and `make distcheck` fails on.
    const std::string other_cert = "mismatch_cert.pem";
    const std::string other_key = "mismatch_key.pem";

    try {
        if(make_cert(other_cert, other_key, "elsewhere", "DNS:elsewhere")) {
            sys::tls_context::server(cert, other_key);
        }
        else {
            threw = true;   // could not set the case up; do not fail for it
            message = "skipped";
        }
    }
    catch(std::exception& e) {
        threw = true;
        message = e.what();
    }

    std::remove(other_cert.c_str());
    std::remove(other_key.c_str());

    ok("a key that does not match the certificate is refused at once", threw,
       message);

    threw = false;

    try { sys::tls_context::server("no-such-file.pem", key); }
    catch(std::exception&) { threw = true; }

    ok("and so is a certificate that is not there", threw);
}

static void a_client_and_a_server_in_one_process(const std::string& cert,
                                                 const std::string& key)
{
    std::cout << "\na client and a server, in one process:\n";

    sys::tls_context ctx = sys::tls_context::server(cert, key);
    sys::listener l;

    std::string saw;
    std::thread server([&l, &ctx, &saw] { echo_once(l, ctx, &saw); });

    try {
        // "localhost", because that is the name the generated certificate
        // covers -- see the SAN in certificate.hh.
        sys::tlsstream client("localhost", l.port());

        client << "hello over TLS\r\n" << std::flush;

        std::string line;
        std::getline(client, line);

        while(!line.empty() && line.back() == '\r') line.pop_back();

        ok("the handshake completes and the line comes back",
           line == "you said: hello over TLS", line);

        ok("and the server read what was sent", saw == "hello over TLS\r" ||
           saw == "hello over TLS", saw);

        client.close();
    }
    catch(std::exception& e) {
        ok("the handshake completes and the line comes back", false, e.what());
    }

    server.join();
}

static void the_certificate_has_to_be_the_right_one(const std::string& cert,
                                                    const std::string& key)
{
    std::cout << "\nthe certificate has to be the right one:\n";

    sys::tls_context ctx = sys::tls_context::server(cert, key);
    sys::listener l;

    bool handshook = false;
    std::thread server([&l, &ctx, &handshook] { echo_once(l, ctx, 0, &handshook); });

    bool threw = false;

    try {
        // 127.0.0.1, where the certificate says DNS:localhost and nothing
        // else.  certificate.hh omits an IP SAN on purpose, which is what
        // makes this provable -- and without this assertion a passing test
        // shows only that bytes moved, not that anything was verified.
        sys::tlsstream client("127.0.0.1", l.port());

        client << "should not get here\r\n" << std::flush;
    }
    catch(std::exception&) { threw = true; }

    ok("a name the certificate does not cover is refused", threw);

    server.join();
}

static void an_untrusted_certificate_is_refused(const std::string& cert,
                                                const std::string& key)
{
    std::cout << "\nan untrusted certificate is refused:\n";

    const char* const had = std::getenv("SSL_CERT_FILE");
    const std::string keep = had ? had : "";

    ::unsetenv("SSL_CERT_FILE");

    sys::tls_context ctx = sys::tls_context::server(cert, key);
    sys::listener l;

    std::thread server([&l, &ctx] { echo_once(l, ctx); });

    bool threw = false;

    try {
        sys::tlsstream client("localhost", l.port());

        client << "should not get here\r\n" << std::flush;
    }
    catch(std::exception&) { threw = true; }

    ok("a certificate nothing trusts is refused", threw);

    server.join();

    if(keep.empty()) ::unsetenv("SSL_CERT_FILE");
    else             ::setenv("SSL_CERT_FILE", keep.c_str(), 1);

    // And it works again once the trust is back, which proves the section
    // above turned something off rather than breaking the arrangement.
    sys::listener again;
    std::string saw;
    std::thread second([&again, &ctx, &saw] { echo_once(again, ctx, &saw); });

    bool worked = false;

    try {
        sys::tlsstream client("localhost", again.port());

        client << "again\r\n" << std::flush;

        std::string line;
        std::getline(client, line);

        worked = line.find("again") != std::string::npos;
    }
    catch(std::exception&) {}

    second.join();

    ok("and accepted again once the trust store is back", worked);
}

static void a_plaintext_client_does_not_take_the_server_with_it(
    const std::string& cert, const std::string& key)
{
    std::cout << "\na plaintext client does not take the server with it:\n";

    sys::tls_context ctx = sys::tls_context::server(cert, key);
    sys::listener l;

    // A handshake failure has to be survivable, because the whole point of a
    // server is that it goes on serving.  This is that assertion at the level
    // of one connection; sys_server_test makes it again at the level of a loop.
    std::string first;
    std::thread server([&l, &ctx, &first] { echo_once(l, ctx, &first); });

    try {
        sys::socketstream plain("127.0.0.1", l.port());

        plain << "GET / HTTP/1.1\r\n\r\n" << std::flush;
        plain.close();
    }
    catch(std::exception&) {}

    server.join();

    ok("the server reports the failure rather than crashing",
       first.find("server threw") == 0, first);

    // Then a real one, on a new listener, with the same context -- which is
    // the part that would break if the failed handshake had damaged it.
    sys::listener again;
    std::string saw;
    std::thread second([&again, &ctx, &saw] { echo_once(again, ctx, &saw); });

    bool worked = false;

    try {
        sys::tlsstream client("localhost", again.port());

        client << "still here\r\n" << std::flush;

        std::string line;
        std::getline(client, line);

        worked = line.find("still here") != std::string::npos;
    }
    catch(std::exception&) {}

    second.join();

    ok("and the same context serves the next connection", worked);
}

int main() {
    std::cout << std::unitbuf;

    const std::string cert = "tls_server_cert.pem";
    const std::string key = "tls_server_key.pem";

    if(!make_cert(cert, key)) {
        std::cerr << "could not generate a test certificate, skipping" << std::endl;

        return 77;
    }

    ::setenv("SSL_CERT_FILE", cert.c_str(), 1);

    a_context_reads_a_certificate(cert, key);
    a_client_and_a_server_in_one_process(cert, key);
    the_certificate_has_to_be_the_right_one(cert, key);
    an_untrusted_certificate_is_refused(cert, key);
    a_plaintext_client_does_not_take_the_server_with_it(cert, key);
    a_slow_server_is_not_a_finished_one(cert, key);
    starttls_refuses_a_stream_with_bytes_in_it();
    starttls_refuses_a_stream_with_bytes_pending();
    a_legitimate_starttls_still_works(cert, key);
    a_ticket_outlives_one_rotation_and_not_two(cert, key);

    std::remove(cert.c_str());
    std::remove(key.c_str());

    // What a green run does not establish.
    //
    // Not a real peer.  Both ends here are jlib, handshaking against a
    // certificate this test generated with the trust store pointed at it, so
    // this proves the accepting path and the verification and says nothing
    // about a public CA chain or a client jlib did not write.  The live tests
    // -- dovecot, nginx, tinyproxy -- are where jlib's *client* meets software
    // somebody else wrote, and none of them exercises this direction.
    //
    // Not client certificates.  A server context here asks for none and would
    // not examine one; mutual TLS is out of scope and deliberately not half
    // implemented.
    //
    // Not the delayed handshake on this side.  tlsstream takes a delay flag for
    // the accepting path, and STARTTLS-as-a-server is not exercised anywhere.
    //
    // Not the protocol floor.  The context asks for TLS 1.2 as a minimum and
    // nothing here proves a 1.1 client would be turned away -- doing so would
    // mean building a deliberately obsolete client by hand, and the assertion
    // that was here instead was a hardcoded true, which proves less than
    // nothing because it looks like coverage.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
