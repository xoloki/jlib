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
#include <jlib/sys/async_tls.hh>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <fcntl.h>
#include <unistd.h>

#include <sstream>

namespace jlib {
namespace sys {

namespace {

/** Whatever OpenSSL has to say about the last failure on this thread. */
std::string why(SSL* ssl, const std::string& what, int ret) {
    std::ostringstream o;

    o << what << " failed";

    switch(SSL_get_error(ssl, ret)) {
    case SSL_ERROR_ZERO_RETURN: o << ": the connection was closed";      break;
    case SSL_ERROR_SYSCALL:     o << ": an I/O error";                   break;
    case SSL_ERROR_SSL:         o << ": a protocol error";               break;
    default: break;
    }

    unsigned long e;
    char buf[128];

    while((e = ERR_get_error()) != 0) {
        ERR_error_string_n(e, buf, sizeof buf);

        o << "; " << buf;
    }

    return o.str();
}

}

/** The reader half.  Its whole content is the loop in want(). */
class tls_reader : public async_reader {
public:
    explicit tls_reader(async_tls& t) : m_tls(t) {}

    task<bool> fill() {
        if(m_closed) co_return false;

        for(;;) {
            ERR_clear_error();

            // **Tried first, not after a wait.**  SSL_read answers out of the
            // SSL's own plaintext buffer when there is any, so the "does the
            // SSL hold data the reactor cannot see" question never has to be
            // asked separately -- and SSL_pending is never called.
            const int n = SSL_read(m_tls.m_ssl, buf(), int(buf_size()));

            if(n > 0) {
                filled(static_cast<std::size_t>(n));

                co_return true;
            }

            const int e = SSL_get_error(m_tls.m_ssl, n);

            if(e == SSL_ERROR_ZERO_RETURN) {
                // close_notify: the peer finished and said so, which is a
                // clean end and distinguishable from a truncation.
                ended();

                co_return false;
            }

            if(e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                if(!co_await m_tls.want(e, "SSL_read")) co_return false;

                continue;
            }

            ended();

            co_return false;
        }
    }

private:
    async_tls& m_tls;
};

/** The writer half. */
class tls_writer : public async_writer {
public:
    explicit tls_writer(async_tls& t) : m_tls(t) {}

    using async_writer::write;

    task<void> write(const char* data, std::size_t n) {
        std::size_t sent = 0;

        while(sent < n) {
            ERR_clear_error();

            // SSL_MODE_ENABLE_PARTIAL_WRITE is off, so this writes all of what
            // it is given or none -- and after WANT_WRITE OpenSSL requires the
            // **same buffer and the same length** on the retry, which is why
            // the arguments are recomputed from `sent` and not advanced by a
            // partial count.  sslstream.hh's sync() carries the same note.
            const int took = SSL_write(m_tls.m_ssl, data + sent,
                                       int(n - sent));

            if(took > 0) {
                sent += static_cast<std::size_t>(took);

                continue;
            }

            const int e = SSL_get_error(m_tls.m_ssl, took);

            if(e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                if(!co_await m_tls.want(e, "SSL_write")) {
                    throw async_tls::exception(
                        "the peer closed with " + std::to_string(n - sent) +
                        " octets still to be written");
                }

                continue;
            }

            throw async_tls::exception(why(m_tls.m_ssl, "SSL_write", took));
        }

        co_return;
    }

private:
    async_tls& m_tls;
};

async_tls::async_tls(reactor& r, int fd, const std::string& verify_host,
                     cancel_token t)
    : m_reactor(r), m_fd(fd), m_token(t), m_verify_host(verify_host)
{
    // A client with no context of its own still gets one per connection, as
    // the blocking path does -- see the note in tls.hh about SSL_CERT_FILE
    // being read when the context is built.
    m_ctx = tls_context::client();

    set_nonblocking();

    m_reader.reset(new tls_reader(*this));
    m_writer.reset(new tls_writer(*this));
}

async_tls::async_tls(reactor& r, int fd, tls_server_t, tls_context ctx,
                     cancel_token t)
    : m_reactor(r), m_fd(fd), m_token(t), m_ctx(std::move(ctx)), m_accept(true)
{
    if(m_ctx.empty()) throw exception("a server handshake with no certificate");

    set_nonblocking();

    m_reader.reset(new tls_reader(*this));
    m_writer.reset(new tls_writer(*this));
}

async_tls::~async_tls() {
    if(m_ssl != 0) SSL_free(m_ssl);

    // Put the descriptor back as it was found.  This does not own it -- it
    // neither opened nor closes it -- so leaving it non-blocking would be a
    // side effect on somebody else's descriptor.
    if(m_saved_flags != -1) ::fcntl(m_fd, F_SETFL, m_saved_flags);
}

void async_tls::set_nonblocking() {
    const int flags = ::fcntl(m_fd, F_GETFL, 0);

    if(flags == -1) throw exception("could not read the descriptor's flags");

    m_saved_flags = flags;

    if(::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK) == -1)
        throw exception("could not make the descriptor non-blocking");
}

task<bool> async_tls::want(int ssl_error, const char* what) {
    // WANT_WRITE on a read is not a mistake: a TLS 1.2 renegotiation or a
    // 1.3 KeyUpdate makes a read produce protocol output.
    const reactor::event_type e =
        ssl_error == SSL_ERROR_WANT_WRITE ? reactor::WRITE : reactor::READ;

    (void)what;

    co_await until_ready(m_reactor, m_fd, e, m_token);

    co_return true;
}

task<void> async_tls::handshake() {
    if(m_up) co_return;

    ERR_clear_error();

    try { m_ssl = m_ctx.new_ssl(); }
    catch(tls_context::exception& e) { throw exception(e.what()); }

    if(SSL_set_fd(m_ssl, m_fd) != 1)
        throw exception(why(m_ssl, "SSL_set_fd", 0));

    if(!m_accept && !m_verify_host.empty()) {
        // The same verification the blocking path does, and for the same
        // reason: verifying the chain without checking the name accepts any
        // valid certificate from any server the trust store covers.
        SSL_set_hostflags(m_ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);

        if(!SSL_set1_host(m_ssl, m_verify_host.c_str()))
            throw exception(why(m_ssl, "SSL_set1_host", 0));

        SSL_set_tlsext_host_name(m_ssl, m_verify_host.c_str());
    }

    for(;;) {
        ERR_clear_error();

        const int r = m_accept ? SSL_accept(m_ssl) : SSL_connect(m_ssl);

        if(r == 1) {
            m_up = true;

            co_return;
        }

        const int e = SSL_get_error(m_ssl, r);

        if(e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            co_await want(e, m_accept ? "SSL_accept" : "SSL_connect");

            continue;
        }

        throw exception(why(m_ssl, m_accept ? "SSL_accept" : "SSL_connect", r));
    }
}

async_reader& async_tls::reader() { return *m_reader; }

async_writer& async_tls::writer() { return *m_writer; }

task<void> async_tls::shutdown() {
    if(m_ssl == 0 || !m_up) co_return;

    for(;;) {
        ERR_clear_error();

        const int r = SSL_shutdown(m_ssl);

        // 1 is a complete two-way shutdown; 0 means ours went out and the
        // peer's has not come back, which is enough -- waiting for theirs is
        // what hangs on a peer that has already gone.
        if(r >= 0) co_return;

        const int e = SSL_get_error(m_ssl, r);

        if(e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            co_await want(e, "SSL_shutdown");

            continue;
        }

        co_return;
    }
}

}
}
