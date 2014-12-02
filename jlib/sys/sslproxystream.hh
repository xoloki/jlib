/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2000 Joe Yandle <jwy@divisionbyzero.com>
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

#ifndef JLIB_SYS_SSLPROXYSTREAM_HH
#define JLIB_SYS_SSLPROXYSTREAM_HH

#include <jlib/sys/proxystream.hh>
#include <glibmm/thread.h>

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

            basic_sslproxybuf(std::string host, unsigned int port, 
                              std::string phost, u_int pport) 
                throw(std::exception)
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
            void open_ssl() throw(std::exception) {
                static bool s_init = false;
                static Glib::Mutex s_init_mutex;
                static Glib::Mutex s_ctx_mutex;
                static SSL_CTX* s_ctx = 0;

                //std::cerr << "locking OpenSSL init mutex" << std::endl;
                s_init_mutex.lock();
                if(!s_init) {
                    //std::cerr << "initializing OpenSSL" << std::endl;
                    s_init = true;
                    SSL_load_error_strings();
                    SSL_library_init();
                }
                //std::cerr << "unlocking OpenSSL init mutex" << std::endl;
                s_init_mutex.unlock();

                int err;
                
                //std::cerr << "locking OpenSSL ctx mutex" << std::endl;
                s_ctx_mutex.lock();
                if(s_ctx == 0) {
                    s_ctx = SSL_CTX_new(SSLv23_client_method());
                }
                s_ctx_mutex.unlock();
                //std::cerr << "unlocking OpenSSL mutex" << std::endl;

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

            basic_sslproxystream(std::string host, unsigned int port,
                                 std::string phost, u_int pport) 
                : basic_proxystream<charT,traitT>()
            {
                this->m_buf=new basic_sslproxybuf<charT,traitT>(host,port,phost,pport);
                this->init(this->m_buf);
            }
            
            void open(std::string host, unsigned int port,
                      std::string phost, u_int pport) 
            {
                this->m_buf=new basic_sslproxybuf<charT,traitT>(host,port,phost,pport);
                this->init(this->m_buf);
            }

        };
    
        typedef basic_sslproxystream< char, std::char_traits<char> > sslproxystream;
        
    }
}


#endif // JLIB_SYS_SSLPROXYSTREAM_HH
