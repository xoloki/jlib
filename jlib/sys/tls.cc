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

    std::string folded(const char* s) {
        std::string out;

        for(const char* p = s; p && *p; p++) {
            out += char(*p >= 'A' && *p <= 'Z' ? *p + ('a' - 'A') : *p);
        }

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
