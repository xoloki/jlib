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

// jlib::net::Pop3 against a real POP3 server.
//
// The IMAP side got this treatment first and the POP3 side did not, so the
// scheme fix that this branch is named for -- "pop3s" was reading as *plain*
// POP3 and choosing port 110 -- was asserted by reading the code and by
// nothing else.  This is the test that was missing.

#include "mailserver.hh"

#include <jlib/net/Pop3.hh>

#include <jlib/util/URL.hh>

#include <iostream>
#include <sstream>
#include <string>

using mailserver::PASSWORD;
using mailserver::impersonating_body;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static std::string show(const std::string& s) {
    std::string out;

    for(char c : s) {
        if(c == '\r')      out += "\\r";
        else if(c == '\n') out += "\\n";
        else               out += c;
    }

    return out;
}

/** A URL for this scheme and port, with the awkward password on it. */
static jlib::util::URL account(const char* scheme, unsigned int port)
{
    jlib::util::URL url(std::string(scheme) + "://joe@localhost:"
                        + std::to_string(port) + "/");

    url.set_pass(PASSWORD);

    return url;
}

int main() {
    std::cout << std::unitbuf;

    {
        std::string out, err;

        try {
            if(jlib::sys::run({ "dovecot", "--version" }, out, err) != 0) {
                std::cerr << "dovecot does not run here; skipping\n";
                return 77;
            }
        }
        catch(std::exception& e) {
            std::cerr << "no dovecot: " << e.what() << "; skipping\n";
            return 77;
        }
    }

    mailserver::server s;

    if(!s.start()) {
        std::cerr << s.log();
        std::cerr << "could not start a server; skipping\n";

        return 77;
    }

    std::cout << "dovecot pop3 on 127.0.0.1:" << s.pop_port
              << ", pop3s on " << s.pop_tls_port << "\n";

    // The scheme, before anything talks to the server.  find("spop") fails on
    // "pop3s" and find("pop") succeeds, so the standard scheme did not merely
    // fail to be secure -- it matched the *plain* branch and chose port 110.
    ok("pop3s:// is a secure scheme",
       jlib::net::Pop3::is_secure(jlib::util::URL("pop3s://joe@localhost/")));
    ok("and so is spop://, the older spelling",
       jlib::net::Pop3::is_secure(jlib::util::URL("spop://joe@localhost/")));
    ok("pop3:// is not",
       !jlib::net::Pop3::is_secure(jlib::util::URL("pop3://joe@localhost/")));
    ok("and neither is pop://",
       !jlib::net::Pop3::is_secure(jlib::util::URL("pop://joe@localhost/")));

    // A scheme that is neither is refused rather than quietly treated as one.
    {
        bool threw = false;

        try { jlib::net::Pop3 p(jlib::util::URL("imap://joe@localhost/")); }
        catch(jlib::net::Pop3::exception&) { threw = true; }

        ok("a scheme that is not POP3 at all is refused", threw);
    }

    // Plaintext first, then TLS on both spellings.  false: leave the mail
    // where it is, so the three runs each see the same maildrop.
    struct { const char* what; const char* scheme; unsigned int port; } runs[] = {
        { "pop3://",  "pop3",  s.pop_port },
        { "pop3s://", "pop3s", s.pop_tls_port },
        { "spop://",  "spop",  s.pop_tls_port },
    };

    for(const auto& r : runs) {
        std::cout << "\n" << r.what << ":\n";

        try {
            jlib::net::Pop3 pop(account(r.scheme, r.port), false);

            // retrieve() connected, disconnected and returned an empty list:
            // the loop was commented out, so the only public way to get mail
            // out of a POP3 account had never got any -- and reported success
            // while doing it.
            const std::list<std::string> mail = pop.retrieve();

            ok("retrieve() returns the maildrop", mail.size() == 2,
               std::to_string(mail.size()) + " messages");

            if(mail.size() != 2) continue;

            std::list<std::string>::const_iterator i = mail.begin();

            ok("the first message is whole",
               *i == "From: a@b.c\r\nSubject: one\r\n\r\nhello\r\n",
               show(*i));

            ++i;

            // The one that matters.  RFC 1939 3 ends a multi-line response
            // with a "." on a line of its own, so a body containing a line
            // that *starts* with a dot is sent with the dot doubled and has to
            // be unstuffed on the way in.  This body has one -- ".signature",
            // which is the case the SMTP side got wrong for twenty-five years.
            ok("and the second survives dot-stuffing", *i == impersonating_body(),
               *i == impersonating_body()
                   ? std::string()
                   : std::to_string(i->size()) + " of "
                     + std::to_string(impersonating_body().size()) + " octets");

            ok("including its leading-dot line",
               i->find("\r\n.signature\r\n") != std::string::npos);
        }
        catch(std::exception& e) {
            ok(std::string(r.what) + " works", false, e.what());
            std::cerr << s.log();
        }
    }

    // The control, as on the IMAP side: read the same message the way the old
    // code did -- to the octet count in the +OK -- and show it lands in the
    // wrong place.
    std::cout << "\nthe count in the +OK:\n";

    {
        std::istringstream advisory(
            "+OK 10 octets\r\n"
            "From: a@b.c\r\n"
            "\r\n"
            "hello\r\n"
            ".\r\n"
            "+OK next command\r\n");

        std::string line;

        std::getline(advisory, line);       // the +OK

        // What retrieve() used to do: tokenize the line and read that many.
        std::string body(10, '\0');

        advisory.read(&body[0], 10);

        ok("reading the advertised count stops in the wrong place",
           body != "From: a@b.c\r\n\r\nhello\r\n", show(body));

        // And what it does now.
        std::istringstream same(
            "+OK 10 octets\r\n"
            "From: a@b.c\r\n"
            "\r\n"
            "hello\r\n"
            ".\r\n"
            "+OK next command\r\n");

        std::getline(same, line);

        // Once, into a variable.  Calling it twice -- as the first draft did,
        // for the assertion and again for the detail -- reads the stream to
        // the end and then throws on the second.
        const std::string whole = jlib::net::Pop3::read_body(same);

        ok("reading to the terminator does not",
           whole == "From: a@b.c\r\n\r\nhello\r\n", show(whole));
    }

    // What a green run does NOT establish.
    //
    // Not interoperability.  One server, one version, one configuration.
    //
    // Not deletion.  Every run above passes false for `remove`, because the
    // three of them share a maildrop and the first would otherwise empty it.
    // So DELE is sent by no test here, and the `remove` flag is exercised only
    // in the direction that does nothing.
    //
    // Not APOP, and not SASL.  jlib sends USER and PASS, which is why this
    // test cares so much about the connection being encrypted first.
    //
    // Not STARTTLS.  jlib does implicit TLS only -- a separate port, TLS from
    // the first byte.  RFC 2595's STLS on the plain port is not implemented,
    // and a server offering only that cannot be used.
    return failures ? 1 : 0;
}
