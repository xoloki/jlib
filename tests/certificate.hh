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

#ifndef JLIB_TESTS_CERTIFICATE_HH
#define JLIB_TESTS_CERTIFICATE_HH

// A self-signed certificate, generated at runtime, for a test that needs a
// real TLS handshake.
//
// Shared by sys_tls_sigpipe_test, which serves TLS in-process, and
// net_imap_live_test, which hands it to a Dovecot.  Both point the client's
// trust store at it with SSL_CERT_FILE -- which sslstream reaches through
// SSL_CTX_set_default_verify_paths -- so nothing about the verification is
// weakened: the handshake is real, the hostname is checked, and it succeeds
// because the certificate is genuinely trusted for the run.

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <cstdio>
#include <string>

/**
 * A self-signed certificate, written to cert_path and key_path.
 *
 * @param cn   the common name
 * @param san  the subject alternative names, in X509V3_EXT_conf_nid syntax
 */
inline bool make_cert(const std::string& cert_path, const std::string& key_path,
                      const std::string& cn = "localhost",
                      const std::string& san = "DNS:localhost") {
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
                               reinterpret_cast<const unsigned char*>(cn.c_str()),
                               -1, -1, 0);
    X509_set_issuer_name(x, name);

    // A subject alternative name, not just a CN: SSL_set1_host checks the SAN,
    // and modern OpenSSL will not fall back to the common name.
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, x, x, 0, 0, 0);

    // DNS:localhost and nothing else, deliberately.  A test that wants to
    // show hostname verification working needs a name the certificate does
    // *not* cover, and 127.0.0.1 is the one every caller here already has --
    // an IP SAN would make it match, because OpenSSL checks iPAddress entries
    // when the name it is given is an address literal.
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(0, &ctx, NID_subject_alt_name,
                                              san.c_str());
    if(ext) { X509_add_ext(x, ext, -1); X509_EXTENSION_free(ext); }

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

#endif // JLIB_TESTS_CERTIFICATE_HH
