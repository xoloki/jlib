/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joe Yandle <jwy@divisionbyzero.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 * 
 */

#ifndef JLIB_CRYPT_CRYPT_HH
#define JLIB_CRYPT_CRYPT_HH

#include <glibmm/refptr.h>

#include <jlib/sys/object.hh>

#include <gpgme.h>

#include <string>
#include <list>
#include <map>

namespace jlib {
    namespace crypt {

        class exception : public std::exception {
        public:
            exception(std::string msg = "") {
                m_msg = std::string("jlib::crypt exception")+( (msg=="")?"":": ")+msg;
            }
            virtual ~exception() throw() {}
            virtual const char* what() const throw() { return m_msg.c_str(); }
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
                exception(std::string s) {
                    m_msg = ("jlib::crypt::gpg::exception: " + s);
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            class eof : public std::exception {
            public:
                virtual const char* what() const throw() { return "jlib::crypt::gpg: EOF"; }
            };

            class ctx;

            class data : public sys::Object {
            public:
                typedef Glib::RefPtr<data> ptr;

                static ptr create(const char* data, size_t n, bool copy = true);
                static ptr create(std::string data);
                static ptr create(std::string file, bool copy);
                static ptr create();

            protected:
                data(const char* data, size_t n, bool copy = true);
                data(std::string data);
                data(std::string file, bool copy);
                data();

                ~data();

            public:
                void set_encoding(gpgme_data_encoding_t e);
                gpgme_data_encoding_t get_encoding();

                std::string read(int n = -1);
                void write(std::string data);

                off_t seek(off_t offset, int whence);
                void rewind();

                friend class ctx;

            protected:
                gpgme_data_t m_data;
            };

            class key : public sys::Object {
            public:
                typedef Glib::RefPtr<key> ptr;
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
                typedef Glib::RefPtr<ctx> ptr;

                ctx();
                ~ctx();

                void set_protocol(gpgme_protocol_t proto = GPGME_PROTOCOL_OpenPGP);
                void set_armor(bool armor = true);
                void set_keylist_mode(int mode);
                void set_passphrase_cb(gpgme_passphrase_cb_t cb, void* hook = 0);
                void set_progress_cb(gpgme_progress_cb_t cb, void* hook = 0);

                void op_keylist_start(std::string ptrn = "", bool secret_only = false);
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
            
            key::list list_keys(std::string id, bool secret_only = false);

            std::string verify(std::string sig);
            std::string verify(std::string sig,std::list<std::string> data);

            std::string decrypt(std::string text, std::string pass);
            std::string decrypt(std::string text, std::string pass, std::string& out);

            std::string sign(std::string from, std::string data, std::string pass);
            std::string encrypt(std::string to, std::string data);
            std::string sign_encrypt(std::string from, std::string to, std::string data, std::string pass);
        }
    }
}

#endif //JLIB_CRYPT_CRYPT_HH
