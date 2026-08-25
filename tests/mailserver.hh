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

#ifndef JLIB_TESTS_MAILSERVER_HH
#define JLIB_TESTS_MAILSERVER_HH

// A real mail server, started for a test and stopped after it.
//
// Dovecot on four high ports -- imap, imaps, pop3, pop3s -- with a Maildir
// this seeds and a certificate generated at runtime.  Shared by
// net_imap_live_test and net_pop3_live_test, which is the only reason it is a
// header: the two need the same server and there is no third caller.
//
// A test using it reports SKIP (exit 77) where there is no dovecot to start,
// which is every machine that is not the build container.
//
// It has to run as root.  Dovecot refuses to start its login process as root,
// so the config leaves that one on the package's own unprivileged user, and
// the seeded Maildir is chowned to an ordinary uid because Dovecot also
// refuses a mail uid of 0.

#include "certificate.hh"

#include <jlib/sys/socketstream.hh>
#include <jlib/sys/sys.hh>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace mailserver {

namespace fs = std::filesystem;


// A password that is exactly what the old code could not send.  A space ends
// an argument, a quote ends a quoted one, and a backslash escapes whatever
// follows -- so "LOGIN " + user + " " + pass produced a malformed command and
// the server said BAD.
inline const char* const PASSWORD = "pa ss \"quoted\" back\\slash";

/**
 * A message body that impersonates every tag jlib is going to use.
 *
 * Imap4::tag() counts up from A00001, so a body containing all twenty of the
 * first tags followed by "OK FETCH completed" looks, line by line, exactly
 * like the completion the client is waiting for.  Reading lines stops at the
 * first of them; reading responses does not.
 *
 * It ends with a line beginning with a dot, which is the POP3 half of the
 * same idea: RFC 1939 ends a multi-line response with a "." on a line of its
 * own, so a body containing one has to be stuffed and unstuffed.
 */
inline std::string impersonating_body()
{
    std::string s = "From: b@c.d\r\nSubject: two\r\n\r\n";

    for(int i = 1; i <= 20; i++) {
        char tag[16];

        std::snprintf(tag, sizeof tag, "A%05d", i);

        s += std::string(tag) + " OK FETCH completed\r\n";
    }

    // A line beginning with a dot, which POP3 and SMTP both have to double on
    // the way out and undouble on the way in -- RFC 1939 3 and RFC 5321 4.5.2.
    // ".signature" is the case the SMTP side got wrong for twenty-five years:
    // it is not a lone dot, so the old code's search for the terminator did
    // not see it at all.
    s += ".signature\r\n";

    return s;
}
struct server {
    fs::path dir;
    unsigned int port = 0;
    unsigned int tls_port = 0;
    unsigned int pop_port = 0;
    unsigned int pop_tls_port = 0;
    bool running = false;

    ~server() { stop(); }

    bool start()
    {
        // A port derived from the pid, so two builds on one machine do not
        // collide.
        port = 14000 + static_cast<unsigned int>(::getpid() % 900);
        tls_port = port + 1000;
        pop_port = port + 2000;
        pop_tls_port = port + 3000;

        dir = fs::temp_directory_path() / ("jlib-imap-" + std::to_string(::getpid()));

        fs::remove_all(dir);
        fs::create_directories(dir / "run");
        fs::create_directories(dir / "mail" / "Maildir" / "cur");
        fs::create_directories(dir / "mail" / "Maildir" / "new");
        fs::create_directories(dir / "mail" / "Maildir" / "tmp");

        // Dovecot refuses a mail uid of 0, and refuses to run imap-login as
        // root at all, so the Maildir belongs to an ordinary uid and the login
        // process keeps the package's own unprivileged user.
        const unsigned int uid = 1000;

        {
            std::ofstream f(dir / "users");

            f << "joe:{PLAIN}" << PASSWORD << ":" << uid << ":" << uid
              << "::" << (dir / "mail").string() << "\n";
        }

        // A certificate for localhost, and the client's trust store pointed
        // at it.  Not a way around the verification -- sslstream still checks
        // the hostname with SSL_set1_host -- but a way to make it succeed
        // without a real CA.  The SAN says DNS:localhost, which is why the TLS
        // side of this test connects to "localhost" and the plain side to
        // 127.0.0.1.
        const std::string cert = (dir / "cert.pem").string();
        const std::string key = (dir / "key.pem").string();

        if(!make_cert(cert, key)) {
            std::cerr << "could not generate a certificate\n";

            return false;
        }

        ::setenv("SSL_CERT_FILE", cert.c_str(), 1);

        {
            std::ofstream f(dir / "conf");

            f << "protocols = imap pop3\n"
              << "listen = 127.0.0.1\n"
              << "base_dir = " << (dir / "run").string() << "\n"
              << "log_path = " << (dir / "log").string() << "\n"
              << "ssl = yes\n"
              << "ssl_cert = <" << cert << "\n"
              << "ssl_key = <" << key << "\n"
              << "disable_plaintext_auth = no\n"
              << "mail_location = maildir:" << (dir / "mail" / "Maildir").string() << "\n"
              << "service imap-login {\n"
              << "  inet_listener imap {\n    port = " << port << "\n  }\n"
              << "  inet_listener imaps {\n    port = " << tls_port
              << "\n    ssl = yes\n  }\n"
              << "  chroot =\n"
              << "}\n"
              << "service pop3-login {\n"
              << "  inet_listener pop3 {\n    port = " << pop_port << "\n  }\n"
              << "  inet_listener pop3s {\n    port = " << pop_tls_port
              << "\n    ssl = yes\n  }\n"
              << "  chroot =\n"
              << "}\n"
              << "passdb {\n  driver = passwd-file\n  args = "
              << (dir / "users").string() << "\n}\n"
              << "userdb {\n  driver = passwd-file\n  args = "
              << (dir / "users").string() << "\n}\n";
        }

        {
            std::ofstream f(dir / "mail" / "Maildir" / "new" / "1", std::ios::binary);
            f << "From: a@b.c\r\nSubject: one\r\n\r\nhello\r\n";
        }

        {
            std::ofstream f(dir / "mail" / "Maildir" / "new" / "2", std::ios::binary);
            f << impersonating_body();
        }

        std::string out, err;

        jlib::sys::run({ "chown", "-R", std::to_string(uid) + ":" + std::to_string(uid),
                         (dir / "mail").string() }, out, err);

        // dovecot daemonizes, so this returns once it has forked.
        if(jlib::sys::run({ "dovecot", "-c", (dir / "conf").string() }, out, err) != 0) {
            std::cerr << "dovecot would not start: " << err << out << "\n";

            return false;
        }

        running = true;

        // Wait for the listener rather than sleeping a guessed interval.
        for(int i = 0; i < 100; i++) {
            try {
                jlib::sys::socketstream imap("127.0.0.1", port);
                jlib::sys::socketstream pop("127.0.0.1", pop_port);

                return true;
            }
            catch(std::exception&) {
                ::usleep(50000);
            }
        }

        std::cerr << "dovecot never listened on " << port << "\n";

        return false;
    }

    void stop()
    {
        if(running) {
            std::string out, err;

            jlib::sys::run({ "dovecot", "-c", (dir / "conf").string(), "stop" }, out, err);
            running = false;
        }

        std::error_code ec;

        fs::remove_all(dir, ec);
    }

    std::string log() const
    {
        std::ifstream f(dir / "log");

        return std::string(std::istreambuf_iterator<char>(f), {});
    }
};

}

#endif // JLIB_TESTS_MAILSERVER_HH
