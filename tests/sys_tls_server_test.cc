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

    try {
        std::string other_cert = "mismatch_cert.pem", other_key = "mismatch_key.pem";

        if(make_cert(other_cert, other_key, "elsewhere", "DNS:elsewhere")) {
            sys::tls_context::server(cert, other_key);
        }
        else {
            threw = true;   // could not set the case up; do not fail for it
            message = "skipped";
        }

        std::remove(other_cert.c_str());
        std::remove(other_key.c_str());
    }
    catch(std::exception& e) {
        threw = true;
        message = e.what();
    }

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
