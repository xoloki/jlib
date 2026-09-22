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

#include <jlib/sys/tls.hh>

#include <openssl/err.h>
#include <map>
#include <memory>
#include <openssl/ssl.h>

namespace jlib {
namespace sys {

namespace {

    /** Whatever OpenSSL has to say about the last failure, drained. */
    std::string why() {
        std::string out;

        for(unsigned long e = ERR_get_error(); e != 0; e = ERR_get_error()) {
            char buf[256];

            ERR_error_string_n(e, buf, sizeof buf);

            if(!out.empty()) out += "; ";

            out += buf;
        }

        return out.empty() ? std::string("no further detail") : out;
    }

}

tls_context::tls_context(SSL_CTX* ctx)
    : m_ctx(ctx, SSL_CTX_free)
{}

tls_context tls_context::client() {
    // OpenSSL 1.1 initializes itself on first use, so there is no library init
    // here and no once-guard; configure.ac requires >= 1.1.
    ERR_clear_error();

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());

    if(ctx == 0) throw exception("SSL_CTX_new: " + why());

    tls_context held(ctx);

    // TLS 1.0 and 1.1 are deprecated by RFC 8996 and no mail or token endpoint
    // needs them.
    if(!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION))
        throw exception("SSL_CTX_set_min_proto_version: " + why());

    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, 0);

    // Read here, which is why there is no cached context: a caller that sets
    // SSL_CERT_FILE for one run -- as three tests in this tree do -- needs the
    // context built after they set it.
    if(!SSL_CTX_set_default_verify_paths(ctx))
        throw exception("SSL_CTX_set_default_verify_paths: " + why());

    return held;
}

tls_context tls_context::server(const std::string& cert_file,
                                const std::string& key_file)
{
    ERR_clear_error();

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());

    if(ctx == 0) throw exception("SSL_CTX_new: " + why());

    tls_context held(ctx);

    if(!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION))
        throw exception("SSL_CTX_set_min_proto_version: " + why());

    // The chain form, not SSL_CTX_use_certificate_file: a certificate signed by
    // an intermediate has to send the intermediate too, or every client that
    // does not already hold it fails to build a path.  It reads a
    // single-certificate file identically, so there is nothing to choose
    // between them for the simple case.
    if(SSL_CTX_use_certificate_chain_file(ctx, cert_file.c_str()) != 1)
        throw exception("could not read the certificate \"" + cert_file +
                        "\": " + why());

    if(SSL_CTX_use_PrivateKey_file(ctx, key_file.c_str(), SSL_FILETYPE_PEM) != 1)
        throw exception("could not read the private key \"" + key_file +
                        "\": " + why());

    // Now, rather than on the first client to connect.  A mismatched pair
    // otherwise fails inside SSL_accept, where it reads as a handshake problem
    // and the certificate is the last thing anyone looks at.
    if(SSL_CTX_check_private_key(ctx) != 1)
        throw exception("the private key \"" + key_file + "\" does not match "
                        "the certificate \"" + cert_file + "\": " + why());

    // No SSL_CTX_set_verify and no client CA list: see the note in the header
    // about client certificates being out of scope.

    // **No renegotiation** (#240).
    //
    // TLS 1.3 has no renegotiation; TLS 1.2 does, and this context allows 1.2
    // because a minimum of 1.3 would refuse clients that are not obsolete.
    // Client-initiated renegotiation is cheap to ask for and expensive to
    // answer -- a full handshake, asymmetric work, at a rate the client
    // chooses -- which is a server doing an attacker's computation for them.
    //
    // Nothing here needs it. Its one real use is asking for a client
    // certificate part-way through a connection, and client certificates are
    // out of scope two comments above. A server that never renegotiates loses
    // nothing by saying so.
    //
    // Server-side only: this is `tls_context::server`, and a *client* that
    // refused renegotiation would be refusing something the peer is entitled
    // to start.
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);

    // **Session tickets: OpenSSL's defaults, deliberately, and what that
    // means for forward secrecy.**  Measured rather than assumed:
    //
    //     SSL_OP_NO_TICKET   not set   -- tickets are issued
    //     max_early_data     0         -- 0-RTT off, so no replay question
    //     num_tickets        2         -- TLS 1.3 issues two per handshake
    //
    // The ticket key belongs to this SSL_CTX and is generated here. It is
    // never rotated while the context lives, which is the forward-secrecy
    // question #240 deferred to #270: somebody who later obtains that key can
    // decrypt any session resumed with a ticket it issued.
    //
    // **The key's lifetime is the context's**, so it rotates whenever a
    // caller builds a new one. jhttpd rebuilds on every SIGHUP, and its
    // logrotate snippet reloads daily -- so in that deployment the window is
    // about a day, which is better than nginx's default of rotating only on
    // reload. That is worth knowing *and* worth distrusting: it is a property
    // of how often something unrelated happens to restart the context, not a
    // guarantee this code makes. A server nobody reloads keeps one key for
    // months.
    //
    // Left on rather than disabled. SSL_OP_NO_TICKET would make resumption
    // impossible and forward secrecy exact, which is the stricter choice --
    // but it is not what Apache or nginx do, 0-RTT is already off so the
    // replay hazard is absent, and a full handshake per connection is a cost
    // paid by every honest client to narrow a window that requires the
    // server's memory to have been read in the first place.
    //
    // If that trade is ever revisited, the middle option is a rotating key
    // via SSL_CTX_set_tlsext_ticket_key_evp_cb, which keeps resumption and
    // bounds the window explicitly instead of incidentally.

    return held;
}

namespace {

    /** The named certificates an SNI callback chooses between. */
    typedef std::map<std::string, std::shared_ptr<SSL_CTX> > site_map;

    void forget_sites(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*) {
        delete static_cast<site_map*>(ptr);
    }

    /**
     * Where the map lives: inside the SSL_CTX, so their lifetimes are the
     * same one rather than two that have to be kept in step.
     *
     * The callback gets an `SSL*` and nothing else useful, so the map has to
     * be reachable from the context it is attached to. ex_data with a free
     * function does exactly that, and means a `tls_context` that is copied and
     * outlives the original still has its sites.
     */
    int sites_index() {
        static const int i =
            SSL_CTX_get_ex_new_index(0, 0, 0, 0, forget_sites);

        return i;
    }

    /**
     * A DNS name as a map key: lowercased, and without the root dot.
     *
     * **Both ends go through this** -- the name a config registered and the
     * name a client asked for -- because a key and a query normalised
     * differently is a site nothing can reach.
     *
     * The dot: RFC 1034 3.1 makes "a.example." the fully qualified form of
     * "a.example", one name. RFC 6066 3 says an SNI HostName MUST NOT carry
     * it, so a conforming client strips it -- but a server that works only
     * for conforming clients fails for the rest, and `Host` accepts the dot
     * at the HTTP layer. Refusing it here while accepting it there makes one
     * URL work over http and fail over https, which is a difference nobody
     * debugs quickly.
     *
     * It also fixes the direction likelier to bite: a config writing
     * `server_name a.example.;` would otherwise key this map with a dot no
     * client sends, and the site would be unreachable with no error anywhere.
     *
     * One dot, not a loop. "a.example.." has an empty label and stays a
     * different name, which is what authority_of does with Host.
     */
    std::string folded(const char* s) {
        std::string out;

        for(const char* p = s; p && *p; p++) {
            out += char(*p >= 'A' && *p <= 'Z' ? *p + ('a' - 'A') : *p);
        }

        if(out.size() > 1 && out[out.size() - 1] == '.')
            out.erase(out.size() - 1);

        return out;
    }

    /**
     * Pick a certificate for the name the client asked for.
     *
     * **Unknown names and absent SNI both keep the default**, rather than
     * failing: see add_site() for why an alert here would be both a free
     * denial of service and a name oracle.
     */
    int choose_site(SSL* ssl, int*, void*) {
        const char* asked =
            SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);

        if(asked == 0) return SSL_TLSEXT_ERR_OK;

        SSL_CTX* ctx = SSL_get_SSL_CTX(ssl);

        if(ctx == 0) return SSL_TLSEXT_ERR_OK;

        const site_map* sites =
            static_cast<const site_map*>(SSL_CTX_get_ex_data(ctx,
                                                             sites_index()));

        if(sites == 0) return SSL_TLSEXT_ERR_OK;

        const site_map::const_iterator i = sites->find(folded(asked));

        if(i == sites->end()) return SSL_TLSEXT_ERR_OK;

        // Switches the certificate chain and key this handshake will send.
        // Every context here was built by server() below, so the protocol
        // floor and the renegotiation policy are the same whichever is
        // chosen -- which matters, because SSL_set_SSL_CTX does not carry
        // across everything a caller might have set afterwards.
        SSL_set_SSL_CTX(ssl, i->second.get());

        return SSL_TLSEXT_ERR_OK;
    }

}

std::time_t tls_context::expires() const {
    if(!m_ctx) return 0;

    // get0: borrowed, not ours to free.
    X509* cert = SSL_CTX_get0_certificate(m_ctx.get());

    if(cert == 0) return 0;

    const ASN1_TIME* when = X509_get0_notAfter(cert);

    if(when == 0) return 0;

    // Against the epoch rather than against now, so a caller decides what
    // "soon" means. ASN1_TIME_diff gives the difference from a reference,
    // and the reference this wants is 1970 -- which it will not take
    // directly, so the difference is taken from now and added back.
    int days = 0;
    int secs = 0;

    if(!ASN1_TIME_diff(&days, &secs, 0, when)) return 0;

    return std::time(0) + std::time_t(days) * 86400 + secs;
}

void tls_context::add_site(const std::string& name,
                           const std::string& cert_file,
                           const std::string& key_file)
{
    if(!m_ctx) throw exception("add_site() on an empty context");

    if(name.empty()) throw exception("add_site() needs a name");

    // Built by the same function as the default, so it carries the same
    // protocol floor and the same refusal to renegotiate.
    const tls_context site = server(cert_file, key_file);

    SSL_CTX* ctx = m_ctx.get();

    site_map* sites =
        static_cast<site_map*>(SSL_CTX_get_ex_data(ctx, sites_index()));

    if(sites == 0) {
        sites = new site_map;

        if(!SSL_CTX_set_ex_data(ctx, sites_index(), sites)) {
            delete sites;

            throw exception("SSL_CTX_set_ex_data: " + why());
        }

        // Installed with the first site rather than in server(), so a context
        // with no sites does not carry a callback that would do nothing.
        SSL_CTX_set_tlsext_servername_callback(ctx, choose_site);
    }

    (*sites)[folded(name.c_str())] = site.m_ctx;
}

SSL* tls_context::new_ssl() const {
    if(!m_ctx) throw exception("new_ssl() on an empty context");

    ERR_clear_error();

    SSL* ssl = SSL_new(m_ctx.get());

    if(ssl == 0) throw exception("SSL_new: " + why());

    return ssl;
}

}
}
