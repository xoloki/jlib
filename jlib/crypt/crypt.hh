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

                static gpgme_key_t* to_array(key::list l);

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

                void op_encrypt(key::list rcpts, data::ptr plain, data::ptr cipher);
                gpgme_encrypt_result_t op_encrypt_result();

                void op_encrypt_sign(key::list rcpts, data::ptr plain, data::ptr cipher);

                gpgme_verify_result_t op_verify(data::ptr sig, data::ptr plain);
                void op_decrypt(data::ptr cipher, data::ptr plain);
                gpgme_verify_result_t op_decrypt_verify(data::ptr cipher, data::ptr plain);

            protected:
                gpgme_ctx_t m_ctx;
                
            };

            void init(gpgme_protocol_t proto = GPGME_PROTOCOL_OpenPGP);
            //GpgmeIdleFunc register_idle(GpgmeIdleFunc idle);
            
            key::list list_keys(const std::string& id, bool secret_only = false);

            std::string verify(const std::string& sig);
            std::string verify(const std::string& sig,std::list<std::string> data);

            std::string decrypt(const std::string& text, const std::string& pass);
            std::string decrypt(const std::string& text, const std::string& pass, std::string& out);

            std::string sign(const std::string& from, const std::string& data, const std::string& pass);
            std::string encrypt(const std::string& to, const std::string& data);
            std::string sign_encrypt(const std::string& from, const std::string& to, const std::string& data, const std::string& pass);
        }
    }
}

#endif //JLIB_CRYPT_CRYPT_HH
