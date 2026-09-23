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
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>
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

namespace {

    /**
     * The key a session ticket is encrypted with, and the one before it.
     *
     * **Rotation, so the forward-secrecy window is stated rather than
     * inherited.** OpenSSL generates one ticket key when an SSL_CTX is built
     * and never changes it, so the window is however long the context lives.
     * For jhttpd that came to "until somebody reloads it" -- daily where
     * logrotate reloads, months otherwise -- and neither was a promise this
     * code made. Somebody who later reads that key out of the process can
     * decrypt every session resumed with a ticket it issued.
     *
     * Two keys, because one would break resumption at every rotation: the
     * current key encrypts, either key decrypts. A ticket arriving under the
     * previous key is accepted *and renewed* -- the 2 returned below -- so a
     * client moves onto the current key without paying for a full handshake.
     *
     * **The interval is the session timeout, measured rather than chosen.**
     * OpenSSL 3.0.13 and 3.6.4 both report 7200 seconds, and a ticket is
     * useless past it, so keeping the previous key for exactly one interval
     * covers every ticket that key issued for the whole of its life.
     * Rotating faster would discard resumptions still inside their timeout;
     * slower widens the window for nothing.
     *
     * Rotation happens on use, not on a timer: a server issuing no tickets
     * has nothing to rotate away from, and a timer would want a reactor this
     * layer does not have.
     */
    struct ticket_keys {
        struct key {
            unsigned char name[16];
            unsigned char aes[32];
            unsigned char hmac[32];
        };

        std::mutex mutex;
        key current;
        key previous;
        bool have_previous = false;
        std::chrono::steady_clock::time_point rotated;
        std::chrono::seconds every;

        explicit ticket_keys(std::chrono::seconds interval) : every(interval)
        {
            fresh(current);

            rotated = std::chrono::steady_clock::now();
        }

        /** New random material.  Throws rather than issue a guessable key. */
        static void fresh(key& k)
        {
            if(RAND_bytes(k.name, sizeof k.name) != 1 ||
               RAND_bytes(k.aes, sizeof k.aes) != 1 ||
               RAND_bytes(k.hmac, sizeof k.hmac) != 1) {
                throw tls_context::exception("RAND_bytes for a session ticket key: " +
                                why());
            }
        }

        /** Called with the mutex held. */
        void rotate_if_due()
        {
            const std::chrono::steady_clock::time_point now =
                std::chrono::steady_clock::now();

            if(now - rotated < every) return;

            previous = current;
            have_previous = true;

            fresh(current);

            rotated = now;
        }
    };

    void forget_ticket_keys(void*, void* ptr, CRYPTO_EX_DATA*, int, long,
                            void*) {
        delete static_cast<ticket_keys*>(ptr);
    }

    /** Where the keys live: in the context, for the reason sites_index says. */
    int ticket_keys_index() {
        static const int i =
            SSL_CTX_get_ex_new_index(0, 0, 0, 0, forget_ticket_keys);

        return i;
    }

    /** The HMAC half, which in OpenSSL 3 is an EVP_MAC rather than a key. */
    bool mac_init(EVP_MAC_CTX* hctx, const unsigned char* key, std::size_t n)
    {
        char digest[] = "SHA256";

        OSSL_PARAM params[2];

        params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,
                                                     digest, 0);
        params[1] = OSSL_PARAM_construct_end();

        return EVP_MAC_init(hctx, key, n, params) == 1;
    }

    /**
     * Issue or accept a session ticket.
     *
     * The OpenSSL contract, which is unusual enough to write down: 1 means
     * use the ticket as it stands, 2 means accept it and issue a replacement,
     * **0 is not an error** -- it is a ticket this server cannot read, and the
     * handshake continues in full -- and negative is a failure.
     */
    int ticket_key_cb(SSL* ssl, unsigned char name[16], unsigned char* iv,
                      EVP_CIPHER_CTX* ctx, EVP_MAC_CTX* hctx, int enc)
    {
        SSL_CTX* c = SSL_get_SSL_CTX(ssl);

        ticket_keys* keys = static_cast<ticket_keys*>(
            SSL_CTX_get_ex_data(c, ticket_keys_index()));

        if(keys == 0) return -1;

        std::lock_guard<std::mutex> hold(keys->mutex);

        if(enc) {
            keys->rotate_if_due();

            if(RAND_bytes(iv, EVP_MAX_IV_LENGTH) != 1) return -1;

            std::memcpy(name, keys->current.name, sizeof keys->current.name);

            if(EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), 0,
                                  keys->current.aes, iv) != 1) {
                return -1;
            }

            return mac_init(hctx, keys->current.hmac,
                            sizeof keys->current.hmac) ? 1 : -1;
        }

        // Which key issued this one?
        const ticket_keys::key* k = 0;
        int answer = 0;

        if(std::memcmp(name, keys->current.name, 16) == 0) {
            k = &keys->current;
            answer = 1;
        }
        else if(keys->have_previous &&
                std::memcmp(name, keys->previous.name, 16) == 0) {
            k = &keys->previous;
            answer = 2;
        }

        if(k == 0) return 0;

        if(!mac_init(hctx, k->hmac, sizeof k->hmac)) return -1;

        if(EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), 0, k->aes, iv) != 1)
            return -1;

        return answer;
    }

} // namespace

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

    // **Session tickets, with a rotating key** (#270, deferred here by #240).
    //
    // Measured rather than assumed, on both toolchains this builds against:
    //
    //     SSL_OP_NO_TICKET   not set   -- tickets are issued
    //     max_early_data     0         -- 0-RTT off, so no replay question
    //     num_tickets        2         -- TLS 1.3 issues two per handshake
    //     session timeout    7200 s    -- and so a ticket's useful life
    //
    // What OpenSSL does on its own is generate one ticket key here and keep
    // it for as long as the context lives. That made the forward-secrecy
    // window a property of something unrelated -- how often anything happens
    // to rebuild the context. jhttpd rebuilds on SIGHUP and its logrotate
    // snippet reloads daily, so *that* deployment had a window of about a
    // day; a server nobody reloads kept one key for months. Either way the
    // window was inherited rather than stated, and the one running six sites
    // had been up continuously since it was installed.
    //
    // Now it is stated: the key rotates every 7200 seconds, the previous key
    // is kept so resumption survives the rotation, and a ticket presented
    // under the previous key is renewed onto the current one. Somebody who
    // reads a key out of this process can decrypt sessions resumed within
    // one interval of it rather than every session since the last restart.
    //
    // **A reload is a harder boundary than a rotation**, and it is worth
    // knowing which one is doing the work. jhttpd's SIGHUP path builds a
    // fresh context to pick up a renewed certificate, so it discards these
    // keys entirely rather than retaining one -- every outstanding ticket
    // stops resuming at a reload, which costs a full handshake per returning
    // client and gives forward secrecy for nothing. Nothing here has to call
    // rotate_ticket_key() for that; the rebuild is what does it. The timer
    // below is what covers the days between reloads, which is where the
    // window used to be unbounded.
    //
    // Tickets stay enabled. SSL_OP_NO_TICKET would make the window exact and
    // resumption impossible, which is a full handshake charged to every
    // honest client to close a gap that already requires reading the
    // server's memory -- and 0-RTT being off means the replay hazard that
    // usually argues against tickets is absent here.
    {
        std::unique_ptr<ticket_keys> keys(
            new ticket_keys(std::chrono::seconds(SSL_CTX_get_timeout(ctx))));

        if(SSL_CTX_set_ex_data(ctx, ticket_keys_index(), keys.get()) != 1)
            throw exception("SSL_CTX_set_ex_data for ticket keys: " + why());

        keys.release();   // owned by the context now, freed by forget_ticket_keys

        if(SSL_CTX_set_tlsext_ticket_key_evp_cb(ctx, ticket_key_cb) != 1)
            throw exception("SSL_CTX_set_tlsext_ticket_key_evp_cb: " + why());
    }

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

bool tls_context::rotate_ticket_key()
{
    if(!m_ctx) return false;

    ticket_keys* keys = static_cast<ticket_keys*>(
        SSL_CTX_get_ex_data(m_ctx.get(), ticket_keys_index()));

    if(keys == 0) return false;

    std::lock_guard<std::mutex> hold(keys->mutex);

    keys->previous = keys->current;
    keys->have_previous = true;

    ticket_keys::fresh(keys->current);

    keys->rotated = std::chrono::steady_clock::now();

    return true;
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
