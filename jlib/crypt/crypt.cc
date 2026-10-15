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

#include <jlib/crypt/crypt.hh>
#include <jlib/sys/sys.hh>
#include <jlib/sys/sync.hh>
#include <jlib/sys/tfstream.hh>

#include <cstdlib>
#include <sstream>

namespace jlib {
    namespace crypt {

        namespace gpg {

            ctx::ctx() {
                gpgme_error_t err = gpgme_new(&m_ctx);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                }
                
            }

            ctx::~ctx() {
                gpgme_release(m_ctx);
            }

            gpgme_ctx_t ctx::gpgme() {
                return m_ctx;
            }

            void ctx::set_protocol(gpgme_protocol_t proto) {
                gpgme_error_t err = gpgme_set_protocol(m_ctx, proto);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                }
            }

            void ctx::set_armor(bool armor) {
                gpgme_set_armor(m_ctx, static_cast<int>(armor));
            }

            void ctx::set_keylist_mode(int mode) {
                gpgme_error_t err = gpgme_set_keylist_mode(m_ctx, mode);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
            }

            void ctx::set_passphrase_cb(gpgme_passphrase_cb_t cb, void* hook) {
                gpgme_set_passphrase_cb(m_ctx, cb, hook);
            }

            void ctx::set_progress_cb(gpgme_progress_cb_t cb, void* hook) {
                gpgme_set_progress_cb(m_ctx, cb, hook);
            }

            void ctx::op_keylist_start(const std::string& ptrn, bool secret_only) {
                const char* pttrn = (ptrn != "" ? ptrn.c_str() : NULL);
                gpgme_error_t err = gpgme_op_keylist_start(m_ctx, pttrn, static_cast<int>(secret_only));

                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
                
            }

            key::ptr ctx::op_keylist_next() {
                key* keyp = new key();
                key::ptr ret(keyp);
                
                gpgme_error_t err = gpgme_op_keylist_next(m_ctx, &keyp->m_key);
                gpgme_err_code_t code = gpgme_err_code(err);

                if(code != GPG_ERR_NO_ERROR) {
                    if(code == GPG_ERR_EOF) {
                        throw eof();
                    } else {
                        throw exception(err);
                    }
                }
                
                return ret;
            }

            void ctx::op_keylist_end() {
                gpgme_error_t err = gpgme_op_keylist_end(m_ctx);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
            }

            void ctx::signers_clear() {
                gpgme_signers_clear(m_ctx);
            }

            void ctx::signers_add(key::ptr key) {
                gpgme_error_t err = gpgme_signers_add(m_ctx, key->gpgme());
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
            }

            void ctx::op_sign(data::ptr plain, data::ptr sign, gpgme_sig_mode_t mode) {
                gpgme_error_t err = gpgme_op_sign(m_ctx, plain->m_data, sign->m_data, mode);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
                sign->rewind();
            }


            void ctx::op_encrypt(const key::list& rcpts, data::ptr plain, data::ptr cipher,
                                 trust t) {
                std::vector<gpgme_key_t> arr = key::to_array(rcpts);
                gpgme_encrypt_flags_t flags =
                    (t == trust::always) ? GPGME_ENCRYPT_ALWAYS_TRUST : gpgme_encrypt_flags_t(0);
                gpgme_error_t err = gpgme_op_encrypt(m_ctx, arr.data(), flags, plain->m_data, cipher->m_data);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
                cipher->rewind();
            }

            gpgme_encrypt_result_t ctx::op_encrypt_result() {
                return gpgme_op_encrypt_result(m_ctx);
            }

            void ctx::op_encrypt_sign(const key::list& rcpts, data::ptr plain, data::ptr cipher,
                                      trust t) {
                std::vector<gpgme_key_t> arr = key::to_array(rcpts);
                gpgme_encrypt_flags_t flags =
                    (t == trust::always) ? GPGME_ENCRYPT_ALWAYS_TRUST : gpgme_encrypt_flags_t(0);
                gpgme_error_t err = gpgme_op_encrypt_sign(m_ctx, arr.data(), flags, plain->m_data, cipher->m_data);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
                cipher->rewind();
            }
            
            gpgme_verify_result_t ctx::op_verify(data::ptr sig, data::ptr plain) {
                gpgme_error_t err = gpgme_op_verify(m_ctx, sig->m_data, 0, plain->m_data);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 

                return gpgme_op_verify_result(m_ctx);
            }

            gpgme_verify_result_t ctx::op_verify_detached(data::ptr sig, data::ptr signed_text) {
                // The signed text goes in gpgme's second argument and the
                // output one is null; op_verify above is the other way round.
                // That argument was hardcoded to null, so this call could not
                // be made at all.
                gpgme_error_t err = gpgme_op_verify(m_ctx, sig->m_data, signed_text->m_data, 0);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 

                return gpgme_op_verify_result(m_ctx);
            }

            void ctx::op_decrypt(data::ptr cipher, data::ptr plain) {
                gpgme_error_t err = gpgme_op_decrypt(m_ctx, cipher->m_data, plain->m_data);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
                plain->rewind();
            }

            gpgme_verify_result_t ctx::op_decrypt_verify(data::ptr cipher, data::ptr plain) {
                gpgme_error_t err = gpgme_op_decrypt_verify(m_ctx, cipher->m_data, plain->m_data);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
                plain->rewind();

                return gpgme_op_verify_result(m_ctx);
            }


            key::key() 
                : m_key(0)
            {
            }

            key::key(gpgme_key_t key) 
                : m_key(key)
            {
                gpgme_key_ref(m_key);
            }

            key::~key() {
                if(m_key)
                    gpgme_key_unref(m_key);
            }

            std::vector<gpgme_key_t> key::to_array(const key::list& l) {
                std::vector<gpgme_key_t> ret;

                ret.reserve(l.size() + 1);

                for(const key::ptr& k : l)
                    ret.push_back(k->gpgme());

                // gpgme reads until the NULL, so the terminator is part of the
                // array and not merely slack.
                ret.push_back(0);

                return ret;
            }

            gpgme_key_t key::gpgme() {
                return m_key;
            }

            data::ptr data::create(const char* d, size_t n, bool copy) {
                return data::ptr(new data(d, n, copy));
            }

            data::ptr data::create(const std::string& d) {
                return data::ptr(new data(d));
            }

            data::ptr data::create(const std::string& file, bool copy) {
                return data::ptr(new data(file, copy));
            }

            data::ptr data::create() {
                return data::ptr(new data());
            }

            data::data(const char* data, size_t n, bool copy) {
                gpgme_error_t err = gpgme_data_new_from_mem(&m_data, data, n, static_cast<int>(copy));
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
            }

            data::data(const std::string& data) {
                gpgme_error_t err = gpgme_data_new_from_mem(&m_data, data.data(), data.length(), 1);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
            }

            data::data(const std::string& file, bool copy) {
                gpgme_error_t err = gpgme_data_new_from_file(&m_data, file.c_str(), static_cast<int>(copy));
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
            }

            data::data() {
                gpgme_error_t err = gpgme_data_new(&m_data);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
            }
            
            data::~data() {
                gpgme_data_release(m_data);
            }
            
            off_t data::seek(off_t offset, int whence) {
                return gpgme_data_seek(m_data, offset, whence);
            }

            void data::rewind() {
                seek(0, SEEK_SET);
            }

            void data::set_encoding(gpgme_data_encoding_t e) {
                gpgme_error_t err = gpgme_data_set_encoding(m_data, e);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 

            }

            gpgme_data_encoding_t data::get_encoding() {
                return gpgme_data_get_encoding(m_data);
            }
            
            std::string data::read(int n) {
                char buf[1024];
                size_t n_read = 0;
                std::string buffer;
                bool cont = true;
                gpgme_error_t err;
                std::string ret;
                ssize_t y = -1;
                
                do {
                    size_t x;
                    if(n == -1) {
                        x = 1024;
                    } else if((n - n_read) > 1024) {
                        x = 1024;
                    } else {
                        x = (n - n_read);
                    }
                    
                    y = gpgme_data_read(m_data, buf, x);

                    if(y == -1)
                        throw exception("gpgme_data_read returned -1");

                    if(y > 0) {
                        n_read += y;
                        ret.append(buf, y);
                    }
                    
                } while((n_read != n) && y != 0);
            
                return ret;
            }

            void data::write(const std::string& data) {
                // ssize_t, not size_t.  gpgme_data_write returns -1 on
                // failure, which as an unsigned became SIZE_MAX -- so the
                // short-write branch caught it first and reported "Only wrote
                // 18446744073709551615", and the errno branch below it was
                // unreachable.
                ssize_t n = gpgme_data_write(m_data, data.data(), data.length());

                if(n < 0)
                    throw exception(errno);

                if(static_cast<std::size_t>(n) != data.size()) {
                    std::ostringstream os;
                    // `<< " of " << data.size()`, not `" of " + size`:
                    // the latter advances a pointer into a six-byte literal
                    // and streams whatever follows it as a C string, which is
                    // out of bounds for any payload over five octets -- in the
                    // path that exists to report a failed write.
                    os << "Only wrote " << n << " of " << data.size();
                    throw std::runtime_error(os.str());
                }
            }

            void init(gpgme_protocol_t proto) {
                // Required before any other gpgme call, and it is the call
                // that initialises the library -- the returned version string
                // was assigned to a variable nobody read.
                gpgme_check_version(0);

                gpgme_set_locale(NULL, LC_CTYPE, setlocale(LC_CTYPE, NULL));

                // proto, not a hardcoded GPGME_PROTOCOL_OpenPGP: the argument
                // was accepted and then ignored, so asking for CMS checked for
                // an OpenPGP engine and reported success on a host with no CMS
                // engine at all.
                gpgme_error_t err = gpgme_engine_check_version(proto);

                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
            }

            key::list list_keys(const std::string& id, bool secret_only) {
                key::list keys;
                ctx ctx;
                
                ctx.set_keylist_mode(1);
                ctx.op_keylist_start(id, secret_only);

                try {
                    while(true) {
                        keys.push_back(ctx.op_keylist_next());
                    }
                } catch(eof& e) {
                    
                }

                return keys;
            }

            bool verified(gpgme_verify_result_t result, trust t) {
                if(result == 0)
                    return false;

                // No signature is not a good signature.  This is the case a
                // caller who only checked whether op_verify threw would have
                // read as success, because an unsigned document verifies
                // without error and reports nothing.
                if(result->signatures == 0)
                    return false;

                // Every flag here disqualifies a signature on its own.  RED is
                // gpgme saying the signature is bad; the rest say it cannot be
                // judged, which is not the same as good.
                const unsigned int disqualifying =
                    GPGME_SIGSUM_RED |
                    GPGME_SIGSUM_KEY_REVOKED |
                    GPGME_SIGSUM_KEY_EXPIRED |
                    GPGME_SIGSUM_SIG_EXPIRED |
                    GPGME_SIGSUM_KEY_MISSING |
                    GPGME_SIGSUM_CRL_MISSING |
                    GPGME_SIGSUM_CRL_TOO_OLD |
                    GPGME_SIGSUM_BAD_POLICY |
                    GPGME_SIGSUM_SYS_ERROR;

                for(gpgme_signature_t sig = result->signatures; sig != 0; sig = sig->next) {
                    if(gpgme_err_code(sig->status) != GPG_ERR_NO_ERROR)
                        return false;

                    if(sig->summary & disqualifying)
                        return false;

                    if(!(sig->summary & GPGME_SIGSUM_GREEN))
                        return false;

                    // GREEN is a well-formed signature by the key it names.
                    // VALID is gpgme saying that key is one the trust model
                    // accepts, which is the question a caller usually means.
                    if(t == trust::required && !(sig->summary & GPGME_SIGSUM_VALID))
                        return false;
                }

                // Every signature present was good, and there was at least one.
                return true;
            }


        }
    }
}

