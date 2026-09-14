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
#ifndef JLIB_SYS_ASYNC_TLS_HH
#define JLIB_SYS_ASYNC_TLS_HH

#include <jlib/sys/async_reader.hh>
#include <jlib/sys/async_writer.hh>
#include <jlib/sys/tls.hh>

#include <memory>
#include <string>

typedef struct ssl_st SSL;

namespace jlib {
namespace sys {

/**
 * One TLS connection, driven by a reactor.
 *
 * Owns the SSL and the descriptor's readiness handling; hands out a reader and
 * a writer that every converted framing function already knows how to use.
 *
 * ## Why this is not basic_tlsbuf
 *
 * basic_tlsbuf is a *streambuf*, and a subclass of basic_socketbuf at that,
 * sharing its descriptor and its get and put areas.  All of that exists to
 * serve std::iostream, and nothing on this path uses iostream: read_head,
 * read_body and imap::read take an async_reader&.  Inheriting from a streambuf
 * to reach an SSL would drag in two buffers nobody reads and the m_delay
 * switch, for nothing.
 *
 * The blocking path is untouched and remains the right thing for a caller that
 * wants a std::iostream.
 *
 * ## The descriptor must be non-blocking, and that is forced
 *
 * Not a preference.  A TLS *record* can arrive in pieces: the socket becomes
 * readable, SSL_read consumes half a record, needs the rest, and on a blocking
 * descriptor **blocks** -- and no amount of checking before the call prevents
 * it, because the shortfall is only discovered inside OpenSSL.
 *
 * So this sets O_NONBLOCK, which is the one place in jlib that does on a
 * connected socket.  The consequences are worth stating:
 *
 * - **A descriptor is either async or blocking, not both.**  The flag belongs
 *   to the descriptor, so a socketstream over the same one would see every
 *   read return EAGAIN.  listener.cc strips the flag from accepted descriptors
 *   for exactly that reason and continues to; this sets it back on the one it
 *   is given, knowingly, and owns it from then on.
 * - The original flags are restored in the destructor, so a caller that wants
 *   the descriptor back gets it as it was.
 *
 * ## Two readiness questions, answered by one call
 *
 * A TLS connection is readable when the socket has octets *or* when the SSL
 * already holds decrypted plaintext nobody has taken.  A reactor answers only
 * the first, so asking it alone waits for data that has already arrived.
 *
 * The fix is to try the read **first** and wait only if OpenSSL says to.  That
 * collapses both questions into SSL_read's return: buffered plaintext comes
 * straight back, and WANT_READ is the only thing that means "now ask the
 * descriptor".  SSL_pending is never called.
 *
 * ## A read can need to write
 *
 * WANT_WRITE on SSL_read is not a mistake: a TLS 1.2 renegotiation or a TLS
 * 1.3 KeyUpdate makes a read produce protocol output.  Both directions are
 * waited on from both operations, which is why one object owns both halves.
 */
class async_tls {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = "async_tls exception: " + msg;
        }
        virtual ~exception() {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }
    protected:
        std::string m_msg;
    };

    /**
     * A client.  The handshake has **not** happened; co_await handshake().
     *
     * Which is the difference from basic_tlsstream, where open_ssl() runs in
     * the constructor.  A constructor cannot be a coroutine, and SSL_connect
     * returns WANT_READ like everything else, so the handshake had to come
     * out.  The cost is that a constructed object is not yet usable and a
     * caller has to know; see handshake().
     *
     * @param verify_host the name the certificate must be good for
     */
    async_tls(reactor& r, int fd, const std::string& verify_host,
              cancel_token t = cancel_token());

    /** A server, with a context that has a certificate. */
    async_tls(reactor& r, int fd, tls_server_t, tls_context ctx,
              cancel_token t = cancel_token());

    ~async_tls();

    async_tls(const async_tls&) = delete;
    async_tls& operator=(const async_tls&) = delete;

    /**
     * Negotiate.  Must complete before reader() or writer() are used.
     *
     * @throws exception if the peer is not who it claims, or the handshake
     *         fails for any other reason
     * @throws cancelled if the token was set
     */
    task<void> handshake();

    bool established() const { return m_up; }

    /** Valid for the life of this object.  Do not use before handshake(). */
    async_reader& reader();
    async_writer& writer();

    /** A close_notify, so the peer can tell this from a truncation. */
    task<void> shutdown();

private:
    friend class tls_reader;
    friend class tls_writer;

    /** Wait for whatever OpenSSL said it needed.  @return false at end. */
    task<bool> want(int ssl_error, const char* what);

    void set_nonblocking();

    reactor&     m_reactor;
    int          m_fd;
    cancel_token m_token;

    tls_context  m_ctx;
    SSL*         m_ssl = 0;

    std::string  m_verify_host;
    bool         m_accept = false;
    bool         m_up = false;

    int          m_saved_flags = -1;

    std::unique_ptr<async_reader> m_reader;
    std::unique_ptr<async_writer> m_writer;
};

}
}

#endif // JLIB_SYS_ASYNC_TLS_HH
