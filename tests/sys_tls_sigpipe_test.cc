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

// The same as sys_sigpipe_test, but over TLS.
//
// Worth having separately because it is a different write path.  OpenSSL calls
// write(2) on the socket itself, so MSG_NOSIGNAL cannot reach it and the plain
// test says nothing about it: on a platform with SO_NOSIGPIPE the socket option
// covers both by construction, and on one without, sigpipe_guard has to be in
// the SSL sync() as well as the plain one.  This is the test that would notice
// if it were not.
//
// Entirely local.  A self-signed certificate for localhost is generated at
// runtime, SSL_CERT_FILE points the client's trust store at it -- which
// basic_sslbuf reaches through SSL_CTX_set_default_verify_paths, and which the
// comment there says is overridable for exactly this sort of reason -- and the
// server side runs in this process.  No network, no external server, and no
// weakening of the verification the client normally does: the handshake is
// real, the hostname is checked, and it succeeds because the certificate is
// genuinely trusted for this run.
#include "certificate.hh"

#include <jlib/sys/listener.hh>
#include <jlib/sys/sslstream.hh>
#include <jlib/sys/sys.hh>

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

static int failures = 0;

static void check(const char* what, long got, long want) {
    const bool ok = (got == want);
    if(!ok) ++failures;
    std::cout << (ok ? "  ok   " : "  FAIL ") << what
              << ": got " << got << ", expected " << want << "\n";
}

int main() {
    const std::string cert = "tls_sigpipe_cert.pem";
    const std::string key  = "tls_sigpipe_key.pem";

    if(!make_cert(cert, key)) {
        std::cerr << "could not generate a test certificate, skipping" << std::endl;
        return 77;
    }

    // What the client will trust, and only for this run.
    ::setenv("SSL_CERT_FILE", cert.c_str(), 1);

    // This used to be twenty lines of socket/bind/listen/getsockname written
    // out here.  sys::listener is that code, generalised, and this is the
    // caller it was generalised from.
    std::unique_ptr<jlib::sys::listener> l;

    try {
        l = std::make_unique<jlib::sys::listener>();
    }
    catch(std::exception& e) {
        std::cerr << "could not set up a loopback socket, skipping: " << e.what() << std::endl;
        std::remove(cert.c_str()); std::remove(key.c_str());
        return 77;
    }

    const unsigned short port = l->port();
    const int lfd = l->get_socket();

    const pid_t pid = ::fork();
    if(pid < 0) {
        std::remove(cert.c_str()); std::remove(key.c_str());
        return 77;
    }

    if(pid == 0) {
        // The client, in the child, because it is the one that might be killed
        // and somebody has to survive to report it.
        ::alarm(30);
        ::close(lfd);

        try {
            jlib::sys::sslstream sock("localhost", port);

            if(!sock)
                ::_exit(3);                 // handshake failed; not what we test

            for(int i = 0; i < 40 && sock; i++) {
                sock << "this goes nowhere\r\n";
                sock.flush();
                ::usleep(20000);            // let the RST get back
            }
        }
        catch(std::exception&) {
            // An exception is a fine outcome; being killed is not.
        }

        ::_exit(0);
    }

    // The server: accept, complete the handshake, then drop the connection.
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    bool served = false;

    if(ctx) {
        SSL_CTX_use_certificate_file(ctx, cert.c_str(), SSL_FILETYPE_PEM);
        SSL_CTX_use_PrivateKey_file(ctx, key.c_str(), SSL_FILETYPE_PEM);

        const int peer = l->accept();
        if(peer >= 0) {
            SSL* ssl = SSL_new(ctx);
            if(ssl) {
                SSL_set_fd(ssl, peer);
                served = (SSL_accept(ssl) == 1);

                // Away without a close_notify, which is how a server that has
                // gone down leaves things.
                SSL_free(ssl);
            }
            ::close(peer);
        }
        SSL_CTX_free(ctx);
    }

    l->close();

    int status = 0;
    ::waitpid(pid, &status, 0);

    std::remove(cert.c_str());
    std::remove(key.c_str());

    if(!served || (WIFEXITED(status) && WEXITSTATUS(status) == 3)) {
        std::cerr << "TLS handshake did not complete, skipping" << std::endl;
        return 77;
    }

    const bool killed = WIFSIGNALED(status) && WTERMSIG(status) == SIGPIPE;

    check("TLS write to a dead peer survives", killed ? 0 : 1, 1);

    return failures ? 1 : 0;
}
