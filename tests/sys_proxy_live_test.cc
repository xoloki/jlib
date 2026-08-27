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

// Reaching a server through an HTTP CONNECT proxy, in the clear and over TLS.
//
// Nothing in this repository had ever exercised that path, and it had never
// worked: open_proxy() called sync() rather than this->sync(), basic_socketbuf
// is a dependent base, and unqualified lookup therefore found ::sync(2) from
// <unistd.h> -- which takes no arguments, returns void, flushes the machine's
// filesystem buffers, and compiles.  The CONNECT request was never sent.
//
// A real proxy is the only way to find that, which is why there is now a
// tinyproxy in the image beside the Dovecot.

#include "mailserver.hh"

#include <jlib/net/Pop3.hh>

#include <jlib/sys/proxystream.hh>
#include <jlib/sys/sslproxystream.hh>
#include <jlib/sys/sslstream.hh>
#include <jlib/sys/sys.hh>

#include <jlib/util/URL.hh>

#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <sstream>
#include <string>

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Did this connect, or did it refuse?  Never lets an exception escape. */
static bool connects(std::function<void()> f) {
    try { f(); return true; }
    catch(std::exception&) { return false; }
}

int main() {
    std::cout << std::unitbuf;

    {
        std::string out, err;

        try {
            if(jlib::sys::run({ "dovecot", "--version" }, out, err) != 0) return 77;
        }
        catch(std::exception& e) {
            std::cerr << "no dovecot: " << e.what() << "; skipping\n";
            return 77;
        }
    }

    mailserver::server s;

    if(!s.start()) {
        std::cerr << s.log() << "could not start a server; skipping\n";
        return 77;
    }

    if(!s.start_proxy()) {
        std::cerr << "no tinyproxy; skipping\n";
        return 77;
    }

    std::cout << "dovecot on " << s.port << "/" << s.tls_port
              << ", proxy on " << s.proxy_port << "\n";

    const std::string host = "127.0.0.1";

    std::cout << "\nCONNECT:\n";

    {
        // The whole of the bug.  Before the fix this read EOF immediately,
        // because the request was still sitting in the put area.
        jlib::sys::proxystream p(host, s.port, host, s.proxy_port);

        std::string line;

        jlib::sys::getline(p, line);

        ok("a plaintext connection through the proxy reaches the server",
           line.find("IMAP4rev1") != std::string::npos, line);

        // tinyproxy answers with a status line *and* a Proxy-agent header.
        // The old reader stopped after the second '\n' it saw and threw the
        // result away, so that header stayed in the stream to be read as the
        // server's greeting.  This assertion is that it did not.
        ok("and the proxy's own headers are not left in the stream",
           line.find("Proxy-agent") == std::string::npos &&
           line.rfind("* OK", 0) == 0, line);

        p << "a1 LOGOUT\r\n" << std::flush;
        jlib::sys::getline(p, line);

        ok("the tunnel carries traffic in both directions",
           line.find("BYE") != std::string::npos, line);
    }

    {
        // A port the proxy will not tunnel to.  It answers 403, which used to
        // be indistinguishable from success -- the status line was read and
        // discarded without being looked at.
        std::string why;

        try {
            jlib::sys::proxystream p(host, s.port + 5000, host, s.proxy_port);
        }
        catch(std::exception& e) { why = e.what(); }

        ok("a refused CONNECT is an error", !why.empty());
        ok("and the message carries the proxy's status line",
           why.find("403") != std::string::npos, why);
    }

    std::cout << "\nTLS through the proxy:\n";

    ok("it connects when the certificate is right",
       connects([&] {
           jlib::sys::tlsproxystream t("localhost", s.tls_port, host, s.proxy_port);
           std::string line;
           jlib::sys::getline(t, line);
       }));

    // The two assertions this branch exists for.  basic_sslproxybuf had its
    // own SSL setup -- SSL_CTX_new, SSL_new, SSL_connect and nothing else.  No
    // SSL_CTX_set_verify, no trust store, no SSL_set1_host.  Both of these
    // connected happily before, which is the worst way for it to be wrong: the
    // connection was encrypted and unauthenticated, and looked fine.
    ok("it refuses a name the certificate does not cover",
       !connects([&] {
           // The certificate carries DNS:localhost and no IP SAN, so the
           // address is a name it is not good for.
           jlib::sys::tlsproxystream t(host, s.tls_port, host, s.proxy_port);
           std::string line;
           jlib::sys::getline(t, line);
       }));

    {
        // And a certificate nothing trusts.  Same test on the direct path
        // first, so a failure here says which of the two is wrong.
        const char* saved = ::getenv("SSL_CERT_FILE");
        const std::string keep = saved ? saved : std::string();

        ::unsetenv("SSL_CERT_FILE");

        ok("the direct path refuses an untrusted certificate",
           !connects([&] {
               jlib::sys::tlsstream t("localhost", s.tls_port);
               std::string line;
               jlib::sys::getline(t, line);
           }));

        ok("and so does the proxied one",
           !connects([&] {
               jlib::sys::tlsproxystream t("localhost", s.tls_port, host, s.proxy_port);
               std::string line;
               jlib::sys::getline(t, line);
           }));

        if(!keep.empty()) ::setenv("SSL_CERT_FILE", keep.c_str(), 1);
    }

    // POP3 through the proxy, which is #103: Pop3::connect had no proxy
    // branch at all, so the URL's parameter was parsed, stored and ignored and
    // a caller who asked for a proxy got a direct connection instead.  Silence
    // is the worst of the three possible behaviours -- they may have asked
    // because it is the only route out.
    {
        std::ostringstream u;

        u << "pop3://joe@127.0.0.1:" << s.pop_port << "/?proxy=127.0.0.1:"
          << s.proxy_port;

        jlib::util::URL url(u.str());

        url.set_pass(mailserver::PASSWORD);

        try {
            // retrieve() is the public way in: it connects, reads the whole
            // maildrop and disconnects.  Captured once -- calling it again for
            // the detail argument would fetch the mail twice, which is the
            // shape that broke the IMAP tests when the second call could not
            // be made on an exhausted stream.
            //
            // false, so the messages are left where they are for whichever
            // test runs next.
            jlib::net::Pop3 pop(url, false);

            const std::list<std::string> mail = pop.retrieve();

            ok("POP3 reaches the server through the proxy", mail.size() == 2,
               std::to_string(mail.size()) + " messages");
        }
        catch(std::exception& e) {
            ok("POP3 reaches the server through the proxy", false, e.what());
        }
    }

    // What a green run does NOT establish.
    //
    // Not proxy authentication.  jlib sends no Proxy-Authorization header and
    // there is nowhere in the URL to put credentials for one, so a proxy that
    // demands them cannot be used -- it will answer 407, which this now
    // reports as an error rather than tunnelling into.
    //
    // Not SOCKS.  basic_proxybuf speaks HTTP CONNECT and only that.
    //
    // Not interoperability.  One proxy, one version.  tinyproxy answers
    // "HTTP/1.0 200 Connection established" and sends one header; a proxy that
    // answers differently, or sends none, is not exercised.
    return failures ? 1 : 0;
}
