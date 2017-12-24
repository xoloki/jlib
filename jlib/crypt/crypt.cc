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

        namespace base64 {
            
            static char decode_table[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 0, 0, 0, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 0, 0, 0, 0, 0, 0, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51  };
            
            int decode(const char* in, char* out) {
                if(in[2] == '=') {
                    out[0] = ((decode_table[in[0]] << 2 ) | (0x03)) & ((0xfc) | (decode_table[in[1]] >> 4));
                    return 1;
                }
                else if(in[3] == '=') {
                    out[0] = ((decode_table[in[0]] << 2 ) | (0x03)) & ((0xfc) | (decode_table[in[1]] >> 4));
                    out[1] = ((decode_table[in[1]] << 4 ) | (0x0f)) & ((0xf0) | (decode_table[in[2]] >> 2));
                    return 2;
                }
                else {
                    out[0] = ((decode_table[in[0]] << 2 ) | (0x03)) & ((0xfc) | (decode_table[in[1]] >> 4));
                    out[1] = ((decode_table[in[1]] << 4 ) | (0x0f)) & ((0xf0) | (decode_table[in[2]] >> 2));
                    out[2] = ((decode_table[in[2]] << 6 ) | (0x3f)) & ((0xc0) | (decode_table[in[3]] >> 0));
                    return 3;
                }
            }
            

            std::string decode(std::string s) {
                char* out = new char[s.length()];
                const char* in = s.data();
                
                int j=0;
                for(unsigned int i=0; i<s.length(); i+=4) {
                    while(i<s.length() && s[i] < 0x20) {
                        i++;
                    }
                    if(i<s.length()) {
                        j += decode(in+i, out+j);
                    }
                }

                std::string ret(out,j);
                delete [] out;
                return ret;
            }
          

            static unsigned char encode_table[] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/' };

            int encode(const char* in, char* out, int sz) {
                if(sz < 1) {
                    return 0;
                }
                else if(sz == 1) {
                    out[0] = encode_table[(in[0] >> 2) & (0x3f)];
                    out[1] = encode_table[(((in[0] << 4) & (0x3f)) | 0x0f) & ((0xf0))];
                    return 2;
                }
                else if(sz == 2) {
                    out[0] = encode_table[(in[0] >> 2) & (0x3f)];
                    out[1] = encode_table[(((in[0] << 4) & (0x3f)) | 0x0f) & ((0xf0) | (in[1] >> 4))];
                    out[2] = encode_table[(((in[1] << 2) & (0x3f)) | 0x03) & ((0xfc))];
                    return 3;
                }
                else if(sz == 3) {
                    out[0] = encode_table[(in[0] >> 2) & (0x3f)];
                    out[1] = encode_table[(((in[0] << 4) & (0x3f)) | 0x0f) & ((0xf0) | (in[1] >> 4))];
                    out[2] = encode_table[(((in[1] << 2) & (0x3f)) | 0x03) & ((0xfc) | (in[2] >> 6))];
                    out[3] = encode_table[(in[2]) & (0x3f)];
                    return 4;
                }
                else {
                    return 0;
                }
            }
            
            std::string encode(std::string s) {
                char* outbuf = new char[2*s.length()];
                int sz = 0;
                u_int j = 0;
                const char* inbuf = s.data();
                
                bool first = true;
                
                while(j < s.length()) {
                    u_int num = 3;
                    if((s.length() - j) < num) {
                        num = s.length() - j;
                    }
                    sz += encode(inbuf+j, outbuf+sz, num);
                    j += 3;
                    if( (first && (sz % 64) == 0) || (!first && ((sz+1) % 65) == 0) ) {
                        outbuf[sz++] = '\n';
                    }
                    if(sz >= 64) {
                        first = false;
                    }
                }
                int numeq = 0;
                switch(s.length() % 3) {
                case 1:
                    numeq = 2;
                    break;
                case 2:
                    numeq = 1;
                    break;
                default:
                    break;
                    
                }
                for(int i = 0; i<numeq; i++) {
                    outbuf[sz++] = '=';
                }
            
                //outbuf[sz] = 0;
                std::string ret(outbuf,sz);
                delete [] outbuf;
                return ret;                
            }
            
        }
        
        namespace qp {
            
            std::string decode(std::string s) {
                std::string ret;
                
                int i=0;
                int j=i;
                
                while((unsigned int)i<s.length() && (j=s.find("=", i)) != -1) {
                    ret += s.substr(i,j-i);
                    if(s[j+1] == '\n') {
                        i = j+2;
                    }
                    else if(s[j+1] == '\r' && s[j+2] == '\n') {
                        i = j+3;
                    }
                    else {
                        std::string ch = s.substr(j+1,2);
                        char val = std::strtol(ch.c_str(), NULL, 16);
                        ret.append(1,val);
                        i = j+3;
                    }
                }
                ret += s.substr(i);
                return ret;
            }

            std::string encode(std::string s) {
                std::string ret;
                
                //TODO: write qp::encode()
                
                return ret;
            }

        }

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

        namespace uri {

            const std::string reserved = ";/?:@&=+$,%";

	    std::string hex_value(unsigned char c, bool upper) {
		const unsigned int size=16;
		char buffer[size];

		snprintf(buffer, size-1, "%02x", c);

		std::string ret(buffer);

		if(upper) {
		    for (unsigned int i = 0; i < ret.size(); i++) {
			ret[i] = toupper(ret[i]);
		    }
		    return ret;
		}
		else {
		    return ret;
		}

	    }


            std::string encode(std::string s) {
                std::string::size_type i, j=0;
                std::string ret = s;
                while( (i=ret.find_first_of(reserved,j)) != std::string::npos ) {
                    std::string tmp = "%" + hex_value(ret[i], false);
                    ret.replace(i,1,tmp);
                    j = i+tmp.length();
                }
                return ret;
            }

            std::string decode(std::string s) {
                std::string ret = s;
                std::string::size_type i;
                while( (i=ret.find("%")) != ret.npos ) {
                    std::string hex = ret.substr(i+1,2);
                    char c = strtol(hex.c_str(), NULL, 16);
                    ret.replace(i,3,1,c);
                }

                return ret;
            }

        }

        namespace xml {
            
            
            

            std::string encode(std::string s) {
                //static jlib::sys::sync< std::map< std::string, std::string> > encmap;
                static jlib::sys::sync< std::map< std::string, std::string> > encmap;

                encmap.lock();
                std::map< std::string, std::string >& encref = encmap.ref();
                if(encref.size() == 0) {
                    encref["&"] = "&amp;";
                    encref["<"] = "&lt;";
                    encref[">"] = "&gt;";
                }
                encmap.unlock();
                
                return recode(s,encref);
            }

            std::string decode(std::string s) {
                static jlib::sys::sync< std::map< std::string, std::string> > decmap;

                decmap.lock();
                std::map< std::string, std::string >& decref = decmap.ref();
                if(decref.size() == 0) {
                    decref["&amp;"] = "&";
                    decref["&lt;"] = "<";
                    decref["&gt;"] = ">";
                }
                decmap.unlock();
                
                return recode(s,decref);
            }

            std::string recode(std::string s, const std::map<std::string,std::string>& codec) {
                std::string ret = s;
                /*
                if(ret.length() > 1) {
                    if( (ret[0] == '"' && ret[ret.length()-1] == '"') ||
                        (ret[0] == '\'' && ret[ret.length()-1] == '\'') ) {
                        ret = ret.substr(1,ret.length()-2);
                    }
                }
                */

                std::map<std::string,std::string>::const_iterator j = codec.begin();
                std::string::size_type i;
                while(j != codec.end()) {
                    while( (i=ret.find(j->first)) != std::string::npos ) {
                        ret.replace(i,j->first.length(),j->second);
                    }
                    j++;
                }
                
                return ret;
                
            }

        }
        
    }
}

