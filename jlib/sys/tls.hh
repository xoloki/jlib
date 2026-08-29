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

#ifndef JLIB_SYS_TLS_HH
#define JLIB_SYS_TLS_HH

#include <exception>
#include <memory>
#include <string>

struct ssl_ctx_st;
struct ssl_st;

namespace jlib {
namespace sys {

/**
 * Tag for the handshake that answers rather than starts.
 *
 * A tag rather than a flag because the two roles are not two settings of one
 * thing: a client verifies a name it was given, a server presents a certificate
 * it holds, and neither has the other's arguments.  Spelling them as separate
 * constructors makes a call site say which it is.
 */
struct tls_server_t { explicit tls_server_t() = default; };
inline constexpr tls_server_t tls_server{};

/**
 * A refcounted SSL_CTX.
 *
 * basic_tlsbuf built one of these per connection and freed it in close(), which
 * is affordable for a client making one call and is not what a server does: a
 * server's certificate and key are read once and every accepted connection
 * SSL_new()s against the same context.  Copying one is a refcount, so a context
 * outlives any connection using it and the last reference frees it.
 *
 * ## Client certificates are out of scope
 *
 * A server context here sets no SSL_VERIFY_PEER and installs no client CA list,
 * so a client that offers a certificate is not asked for one and one offered
 * anyway is not examined.  Mutual TLS is a different feature with a different
 * API, and this is deliberately not half of it -- an unexercised verification
 * path that looks like it works is worse than none.
 *
 * ## There is no cached default client context, on purpose
 *
 * client() builds a new one every time, and that is not an oversight to be
 * optimised away later.  SSL_CTX_set_default_verify_paths reads SSL_CERT_FILE
 * *when the context is built*, and three tests in this tree install trust for
 * one run by setting that variable at runtime -- see tests/certificate.hh.  A
 * context cached at first use would freeze the trust store at whatever the
 * first connection in the process happened to see, and the failure would look
 * like a certificate problem rather than a caching one.
 */
class tls_context {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = "tls_context exception: " + msg;
        }
        virtual ~exception() {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }
    protected:
        std::string m_msg;
    };

    /** Absent.  For a client this means "make the usual one per connection". */
    tls_context() = default;

    /**
     * What basic_tlsbuf::open_ssl() used to build inline: TLS_client_method,
     * TLS 1.2 as the floor, SSL_VERIFY_PEER, and the default verify paths.
     */
    static tls_context client();

    /**
     * A server identity: that certificate chain, that private key, both PEM.
     *
     * Uses SSL_CTX_use_certificate_chain_file rather than the single-certificate
     * form, because a real server has intermediates -- and the chain form reads
     * a one-certificate file too, so nothing is given up.
     *
     * The key is checked against the certificate here, so a mismatched pair is
     * a failure now rather than a handshake failure on the first client to
     * arrive, which is a much harder thing to read.
     */
    static tls_context server(const std::string& cert_file,
                              const std::string& key_file);

    bool empty() const { return !m_ctx; }
    explicit operator bool() const { return static_cast<bool>(m_ctx); }

    /** Borrowed; null when empty(). */
    ssl_ctx_st* get() const { return m_ctx.get(); }

    /** SSL_new against it.  Throws rather than handing back a null. */
    ssl_st* new_ssl() const;

private:
    explicit tls_context(ssl_ctx_st* ctx);      // takes ownership

    std::shared_ptr<ssl_ctx_st> m_ctx;          // deleter is SSL_CTX_free
};

}
}

#endif // JLIB_SYS_TLS_HH
