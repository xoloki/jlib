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

    return held;
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
