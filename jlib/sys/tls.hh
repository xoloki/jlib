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

    /**
     * Serve a different certificate to clients that ask for `name`.
     *
     * The name a client sends in SNI, chosen **during the handshake** -- which
     * is the only moment it can be, because the certificate goes out before
     * the request that carries `Host` arrives. A server with two names on one
     * port needs one of these per name or it presents the wrong certificate
     * and the client stops before saying anything.
     *
     * Exact and case-insensitive, matching `http::server::site_of`: SNI
     * carries a DNS name and DNS names are not case-sensitive. No wildcards,
     * for the reason vhosts have none -- a wildcard is a second way to pick
     * the wrong identity, and the safe version needs rules about labels.
     *
     * ## A name this does not know keeps the default certificate
     *
     * It does **not** fail the handshake, and that is deliberate twice over.
     * A client that can make a handshake fail by naming something is a client
     * that can make the server do asymmetric work for nothing; and an alert
     * that fires only for unknown names is an oracle for which names exist.
     * The mismatch is the client's to notice -- it is the one holding the
     * certificate and the name it asked for.
     *
     * A client that sends no SNI at all also gets the default, which is what
     * every client did before the extension existed.
     *
     * The name is matched case-insensitively and without a trailing root dot,
     * at both ends -- the name registered here and the name a client asks
     * for. RFC 6066 3 says a client MUST NOT send the dot, but `Host` accepts
     * it at the HTTP layer and a server that disagreed with itself would make
     * one URL work over http and fail over https.
     *
     * ## **SNI chooses the certificate. It does not choose the content.**
     *
     * The name a client puts in SNI reaches this callback and goes no
     * further: nothing carries it up to whatever is reading the request, so
     * nothing can compare it with `Host`. A client may take one site's
     * certificate and then ask for another site's pages, and will get them.
     *
     * That is deliberate and it is what nginx does; Apache calls the
     * alternative `SSLStrictSNIVHostCheck` and leaves it off. It is safe
     * because authorisation does not rest on the certificate: a guard is
     * keyed on the name in the request, so a guarded site stays guarded
     * whichever certificate the connection was established with.
     *
     * What it does mean is that **"reachable only over its own certificate"
     * is not a property this offers.** Requiring the two to agree would need
     * the SNI name plumbed up to the request layer, which nothing does today.
     *
     * @throws exception if the pair cannot be read or does not match
     */
    void add_site(const std::string& name, const std::string& cert_file,
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
