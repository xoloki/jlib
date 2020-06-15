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

            void ctx::op_keylist_start(std::string ptrn, bool secret_only) {
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


            void ctx::op_encrypt(key::list rcpts, data::ptr plain, data::ptr cipher) {
                gpgme_key_t* arr = key::to_array(rcpts);
                gpgme_error_t err = gpgme_op_encrypt(m_ctx, arr, GPGME_ENCRYPT_ALWAYS_TRUST, plain->m_data, cipher->m_data);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
                cipher->rewind();
            }

            gpgme_encrypt_result_t ctx::op_encrypt_result() {
                return gpgme_op_encrypt_result(m_ctx);
            }

            void ctx::op_encrypt_sign(key::list rcpts, data::ptr plain, data::ptr cipher) {
                gpgme_key_t* arr = key::to_array(rcpts);
                gpgme_error_t err = gpgme_op_encrypt_sign(m_ctx, arr, GPGME_ENCRYPT_ALWAYS_TRUST, plain->m_data, cipher->m_data);
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

            gpgme_key_t* key::to_array(key::list l) {
                gpgme_key_t* ret = new gpgme_key_t[l.size() + 1];
                unsigned int x = 0;
                key::list::iterator i = l.begin();

                for(; i != l.end(); i++, x++) {
                    ret[x] = (*i)->gpgme();
                }
                ret[x] = 0;

                return ret;
            }

            gpgme_key_t key::gpgme() {
                return m_key;
            }

            data::ptr data::create(const char* d, size_t n, bool copy) {
                return data::ptr(new data(d, n, copy));
            }

            data::ptr data::create(std::string d) {
                return data::ptr(new data(d));
            }

            data::ptr data::create(std::string file, bool copy) {
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

            data::data(std::string data) {
                gpgme_error_t err = gpgme_data_new_from_mem(&m_data, data.data(), data.length(), 1);
                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
            }

            data::data(std::string file, bool copy) {
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

            void data::write(std::string data) {
                std::size_t n = gpgme_data_write(m_data, data.data(), data.length());
                if(n != data.size()) {
                    std::ostringstream os;
                    os << "Only wrote " << n << " of " + data.size();
                    throw std::runtime_error(os.str());
                }
                    

                if(n == -1) {
                    throw exception(errno);
                } 
            }

            void init(gpgme_protocol_t proto) {
                const char* version = gpgme_check_version(0);

                gpgme_set_locale (NULL, LC_CTYPE, setlocale (LC_CTYPE, NULL));

                gpgme_error_t err = gpgme_engine_check_version(GPGME_PROTOCOL_OpenPGP);

                if(gpgme_err_code(err) != GPG_ERR_NO_ERROR) {
                    throw exception(err);
                } 
            }

            /*
            GpgmeIdleFunc register_idle(GpgmeIdleFunc idle) {
                return gpgme_register_idle(idle);
            }
            */

            key::list list_keys(std::string id, bool secret_only) {
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

            std::string verify(std::string sig) {
                return "";
            }

            std::string verify(std::string sig,std::list<std::string> data) {
                return "";
            }

            std::string decrypt(std::string text, std::string pass) {
                return "";
            }

            std::string decrypt(std::string text, std::string pass, std::string& out) {
                return "";
            }

            std::string sign(std::string from, std::string data, std::string pass) {
                return "";
            }

            std::string encrypt(std::string to, std::string data) {
                return "";
            }

            std::string sign_encrypt(std::string from, std::string to, std::string data, std::string pass) {
                return "";
            }


        }
    }
}

