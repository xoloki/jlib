/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_CRYPT_CRYPT_HH
#define JLIB_CRYPT_CRYPT_HH

#include <memory>

#include <jlib/sys/object.hh>

#include <gpgme.h>

#include <string>
#include <list>
#include <map>
#include <vector>

namespace jlib {
    namespace crypt {

        class exception : public std::exception {
        public:
            exception(const std::string& msg = "") {
                m_msg = std::string("jlib::crypt exception")+( (msg=="")?"":": ")+msg;
            }
            virtual ~exception() {}
            virtual const char* what() const noexcept { return m_msg.c_str(); }
        protected:
            std::string m_msg;
        };


        namespace gpg {

            class exception : public std::exception {
            public:
                exception(gpgme_error_t err) {
                    m_msg = std::string("jlib::crypt::gpg::exception: ") + 
                        std::string(gpg_strsource(err)) + ":" +
                        std::string(gpg_strerror(err));
                }
                exception(const std::string& s) {
                    m_msg = ("jlib::crypt::gpg::exception: " + s);
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            class eof : public std::exception {
            public:
                virtual const char* what() const noexcept { return "jlib::crypt::gpg: EOF"; }
            };

            /**
             * Whether an operation may proceed on a key the trust model has
             * not accepted.
             *
             * Encryption passed GPGME_ENCRYPT_ALWAYS_TRUST unconditionally,
             * which is exactly this bypass: it encrypts to whatever key was
             * handed over, so a substituted key is indistinguishable from the
             * right one and the web of trust decides nothing.  It is now a
             * parameter, and it defaults to off.
             */
            enum class trust {
                required,   ///< the trust model must accept the key
                always,     ///< proceed regardless; GPGME_ENCRYPT_ALWAYS_TRUST
            };

            class ctx;

            class data : public sys::Object {
            public:
                typedef std::shared_ptr<data> ptr;

                static ptr create(const char* data, size_t n, bool copy = true);
                static ptr create(const std::string& data);
                static ptr create(const std::string& file, bool copy);
                static ptr create();

            protected:
                data(const char* data, size_t n, bool copy = true);
                data(const std::string& data);
                data(const std::string& file, bool copy);
                data();

            public:
                // std::shared_ptr forms its deleter outside this class, so the
                // destructor has to be reachable from there.  It was protected
                // only because Glib::RefPtr deleted from inside the object via
                // unreference(); the protected constructors above are what
                // actually keep construction funnelled through create().  key
                // and ctx below already expose theirs, and the sys::Object base
                // has a public virtual destructor regardless.
                ~data();

                void set_encoding(gpgme_data_encoding_t e);
                gpgme_data_encoding_t get_encoding();

                std::string read(int n = -1);
                void write(const std::string& data);

                off_t seek(off_t offset, int whence);
                void rewind();

                friend class ctx;

            protected:
                gpgme_data_t m_data;
            };

            class key : public sys::Object {
            public:
                typedef std::shared_ptr<key> ptr;
                typedef std::list<ptr> list;

                key();
                key(gpgme_key_t key);
                ~key();

                /**
                 * The NULL-terminated key array gpgme wants, as a vector
                 * whose data() is that array.  It used to return a bare
                 * `new gpgme_key_t[]` which neither caller ever deleted, so
                 * every encryption leaked it.
                 */
                static std::vector<gpgme_key_t> to_array(const key::list& l);

                gpgme_key_t gpgme();

                friend class ctx;

            protected:
                gpgme_key_t m_key;
            };

            class ctx : public sys::Object {
            public:
                typedef std::shared_ptr<ctx> ptr;

                ctx();
                ~ctx();

                void set_protocol(gpgme_protocol_t proto = GPGME_PROTOCOL_OpenPGP);
                void set_armor(bool armor = true);
                void set_keylist_mode(int mode);
                void set_passphrase_cb(gpgme_passphrase_cb_t cb, void* hook = 0);
                void set_progress_cb(gpgme_progress_cb_t cb, void* hook = 0);

                void op_keylist_start(const std::string& ptrn = "", bool secret_only = false);
                key::ptr op_keylist_next();
                void op_keylist_end();

                void signers_clear();
                void signers_add(key::ptr key);
                void op_sign(data::ptr plain, data::ptr sig, gpgme_sig_mode_t mode = GPGME_SIG_MODE_NORMAL);

                void op_encrypt(const key::list& rcpts, data::ptr plain, data::ptr cipher,
                                trust t = trust::required);
                gpgme_encrypt_result_t op_encrypt_result();

                void op_encrypt_sign(const key::list& rcpts, data::ptr plain, data::ptr cipher,
                                     trust t = trust::required);

                /**
                 * Verify an inline signature, writing the recovered message to
                 * plain.  Returns the verify result, which is evidence and not
                 * a verdict -- pass it to verified() for the verdict.
                 *
                 * gpgme_op_verify fails only for operational reasons: a
                 * malformed packet, a missing engine.  A signature that is
                 * simply bad is a *successful* verify operation carrying a bad
                 * signature in its result, so this returning without throwing
                 * says nothing whatever about validity.
                 *
                 * Hand it a detached signature and the demonstration is right
                 * there: the operation succeeds and the result carries no
                 * signatures at all.
                 */
                gpgme_verify_result_t op_verify(data::ptr sig, data::ptr plain);

                /**
                 * Verify a detached signature over signed_text.
                 *
                 * This was not expressible.  op_verify passed gpgme's
                 * signed_text argument as null unconditionally, which is the
                 * inline form, so a detached signature -- what a mail client
                 * and a release tarball both use -- was verified against
                 * nothing: a successful operation attesting to nothing, which
                 * a caller reading only the absence of an exception would have
                 * taken for a good signature.
                 */
                gpgme_verify_result_t op_verify_detached(data::ptr sig, data::ptr signed_text);
                void op_decrypt(data::ptr cipher, data::ptr plain);

                /** As op_verify: the result still has to go through verified(). */
                gpgme_verify_result_t op_decrypt_verify(data::ptr cipher, data::ptr plain);

                /**
                 * The underlying gpgme context, for the operations this class
                 * does not wrap -- key generation, export and import among
                 * them.  key::gpgme() is here for the same reason, and jcrypt
                 * reads can_sign and can_encrypt through it.
                 */
                gpgme_ctx_t gpgme();

            protected:
                gpgme_ctx_t m_ctx;
                
            };

            /** Initialise gpgme and check that an engine for proto is present. */
            void init(gpgme_protocol_t proto = GPGME_PROTOCOL_OpenPGP);

            key::list list_keys(const std::string& id, bool secret_only = false);

            /**
             * The verdict on a verify result: whether it attests to anything.
             *
             * Walks result->signatures and requires every signature to be
             * good -- a GPG_ERR_NO_ERROR status, GPGME_SIGSUM_GREEN set, and
             * none of RED, KEY_REVOKED, KEY_EXPIRED, SIG_EXPIRED, KEY_MISSING,
             * CRL_MISSING, CRL_TOO_OLD, BAD_POLICY or SYS_ERROR.  With
             * trust::required it also demands GPGME_SIGSUM_VALID, which is
             * gpgme saying the signing key is one the trust model accepts and
             * not merely one that made a well-formed signature.
             *
             * An empty signature list is false.  No signature is not a good
             * signature, and that is precisely the case a caller who only
             * checked whether op_verify threw would have read as success.
             */
            bool verified(gpgme_verify_result_t result, trust t = trust::required);

            /*
             * Removed: verify(), decrypt(), sign(), encrypt() and
             * sign_encrypt() as free functions taking and returning strings.
             * All seven were `return "";` with no implementation behind them,
             * which is the worst available answer in this particular file: a
             * caller asking "is this signature good" got an empty string, and
             * one asking to decrypt got silence instead of plaintext.  Nothing
             * called them.  Use ctx directly, with verified() for the verdict.
             */
        }
    }
}

#endif //JLIB_CRYPT_CRYPT_HH
