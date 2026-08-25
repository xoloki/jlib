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
#include <string>

static int failures = 0;

static void check(const char* what, long got, long want) {
    const bool ok = (got == want);
    if(!ok) ++failures;
    std::cout << (ok ? "  ok   " : "  FAIL ") << what
              << ": got " << got << ", expected " << want << "\n";
}

/** A self-signed certificate for localhost, written to cert_path and key_path. */
static bool make_cert(const std::string& cert_path, const std::string& key_path) {
    EVP_PKEY* key = EVP_RSA_gen(2048);
    if(key == 0) return false;

    X509* x = X509_new();
    if(x == 0) { EVP_PKEY_free(key); return false; }

    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 60 * 60);
    X509_set_pubkey(x, key);

    X509_NAME* name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("localhost"),
                               -1, -1, 0);
    X509_set_issuer_name(x, name);

    // A subject alternative name, not just a CN: SSL_set1_host checks the SAN,
    // and modern OpenSSL will not fall back to the common name.
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x, x, 0, 0, 0);

    X509_EXTENSION* san = X509V3_EXT_conf_nid(0, &ctx, NID_subject_alt_name,
                                              "DNS:localhost,IP:127.0.0.1");
    if(san) { X509_add_ext(x, san, -1); X509_EXTENSION_free(san); }

    if(!X509_sign(x, key, EVP_sha256())) { X509_free(x); EVP_PKEY_free(key); return false; }

    bool ok = false;
    FILE* cf = std::fopen(cert_path.c_str(), "wb");
    FILE* kf = std::fopen(key_path.c_str(), "wb");
    if(cf && kf)
        ok = PEM_write_X509(cf, x) && PEM_write_PrivateKey(kf, key, 0, 0, 0, 0, 0);
    if(cf) std::fclose(cf);
    if(kf) std::fclose(kf);

    X509_free(x);
    EVP_PKEY_free(key);
    return ok;
}

static int listener(unsigned short* port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return -1;

    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;

    if(::bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) < 0) return -1;
    if(::listen(fd, 1) < 0) return -1;

    socklen_t len = sizeof(a);
    if(::getsockname(fd, reinterpret_cast<struct sockaddr*>(&a), &len) < 0) return -1;

    *port = ntohs(a.sin_port);
    return fd;
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

    unsigned short port = 0;
    const int lfd = listener(&port);
    if(lfd < 0) {
        std::cerr << "could not set up a loopback socket, skipping" << std::endl;
        std::remove(cert.c_str()); std::remove(key.c_str());
        return 77;
    }

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

        const int peer = ::accept(lfd, 0, 0);
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

    ::close(lfd);

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
