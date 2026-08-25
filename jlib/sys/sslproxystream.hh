/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_SYS_SSLPROXYSTREAM_HH
#define JLIB_SYS_SSLPROXYSTREAM_HH

#include <jlib/sys/proxystream.hh>

#include <openssl/ssl.h>

#include <sstream>

namespace jlib {
    namespace sys {

        template< typename charT, typename traitT = std::char_traits<charT> >
        class basic_sslproxybuf : public basic_proxybuf<charT,traitT> {
        public:
            typedef charT 					            char_type;
            typedef traitT 					            traits_type;
            typedef typename traits_type::int_type 		int_type;
            typedef typename traits_type::pos_type 		pos_type;
            typedef typename traits_type::off_type 		off_type;
            
            static const unsigned int BUF_SIZE = 1024;

            basic_sslproxybuf(const std::string& host, unsigned int port, 
                              const std::string& phost, u_int pport)
                : basic_proxybuf<charT,traitT>(host,port,phost,pport)
            {
                open_ssl();
            }

            virtual ~basic_sslproxybuf() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_sslbuf::~basic_sslproxybuf()"<<std::endl;
                close();
            }

            virtual int_type underflow() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_sslproxybuf::underflow()"<<std::endl;

                this->m_eintr = false;
                int count = SSL_read(m_ssl, this->eback(), BUF_SIZE);
                
                if(count < 0) {
                    //throw exception("error reading");
                    //this->setstate(std::ios_base::badbit);
                    if(errno == EINTR) {
                        this->m_eintr = true;
                    }
                    std::cerr <<"exception in jlib::sys::sslproxystream::underflow()"<<std::endl;
                    return traits_type::eof();
                }                
                else if(count == 0) {
                    return traits_type::eof();
                }
                else {
                    char_type* end = this->eback()+count;
                    this->setg(this->eback(), this->eback(), end);
                    
                    return traits_type::to_int_type(*this->gptr());
                }
            }

            virtual int_type sync() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_sslproxybuf::sync()"<<std::endl;
                int sofar = 0;
                int total = this->pptr() - this->pbase();
                int diff;
                int count;
                char_type* current = this->pbase();
                
                while( (diff=(total-sofar)) > 0 ) {
                    this->m_eintr = false;
                    count = SSL_write(m_ssl, current, diff);

                    if(count == -1) {
                        if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                            std::cerr <<"exception in jlib::sys::sslproxystream::sync()"<<std::endl;
                        if(errno == EINTR) {
                            this->m_eintr = true;
                        }
                        return traits_type::eof();
                    }
                    sofar += count;
                    current += count;
                }
                
                this->setp(this->pbase(), this->pbase()+BUF_SIZE);
                return 0;                
            }

            virtual void close() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_sslproxybuf::close()"<<std::endl;
                if(m_ssl != 0) {
                    SSL_shutdown(m_ssl);
                    SSL_free(m_ssl);
                    m_ssl = 0;
                }
                basic_proxybuf<charT,traitT>::close();
            }

        protected:
            void open_ssl() {
                // OpenSSL 1.1 initializes itself on first use: SSL_library_init
                // and SSL_load_error_strings became no-ops there and are gone in
                // 3.0.  A function-local static is initialized exactly once and
                // thread-safely as of C++11, which retires both of the
                // hand-rolled mutexes this used to need.  SSLv23_client_method
                // is the deprecated spelling of TLS_client_method.
                static SSL_CTX* s_ctx = SSL_CTX_new(TLS_client_method());

                int err;

                if(s_ctx == 0) {
                    std::cerr <<"exception in jlib::sys::sslproxystream::open_ssl()"<<std::endl;
                    throw typename basic_socketbuf<charT, traitT>::exception("error calling SSL_CTX_new()");
                }
                
                m_ssl = SSL_new(s_ctx);
                if(m_ssl == 0) {
                    std::cerr <<"exception in jlib::sys::sslproxystream::open_ssl()"<<std::endl;
                    throw typename basic_socketbuf<charT, traitT>::exception("error calling SSL_new()");
                }
                
                err = SSL_set_fd(m_ssl,this->m_sock);
                if(err <= 0) {
                    std::ostringstream o;
                    std::cerr <<"exception in jlib::sys::sslproxystream::open_ssl()"<<std::endl;
                    o << "error in SSL_set_fd("<<m_ssl<<","<<this->m_sock<<")";
                    throw typename basic_socketbuf<charT, traitT>::exception(o.str());
                }
                
                err = SSL_connect(m_ssl);
                
                if(err <= 0) {
                    std::ostringstream o;
                    std::cerr <<"exception in jlib::sys::sslproxystream::open_ssl()"<<std::endl;
                    o << "error in SSL_connect("<<m_ssl<<")";
                    throw typename basic_socketbuf<charT, traitT>::exception(o.str());
                }
            }

            
            SSL* m_ssl;
        };
        
        template<typename charT, typename traitT=std::char_traits<charT> >
        class basic_sslproxystream : public basic_proxystream<charT,traitT> {
        public:
            basic_sslproxystream()
                : basic_proxystream<charT,traitT>()
            {}

            basic_sslproxystream(const std::string& host, unsigned int port,
                                 const std::string& phost, u_int pport) 
                : basic_proxystream<charT,traitT>()
            {
                this->m_buf=new basic_sslproxybuf<charT,traitT>(host,port,phost,pport);
                this->init(this->m_buf);
            }
            
            void open(const std::string& host, unsigned int port,
                      const std::string& phost, u_int pport) 
            {
                this->m_buf=new basic_sslproxybuf<charT,traitT>(host,port,phost,pport);
                this->init(this->m_buf);
            }

        };
    
        typedef basic_sslproxystream< char, std::char_traits<char> > sslproxystream;
        
    }
}


#endif // JLIB_SYS_SSLPROXYSTREAM_HH
