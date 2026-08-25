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

#ifndef JLIB_SYS_SOCKETSTREAM_HH
#define JLIB_SYS_SOCKETSTREAM_HH

#include <iostream>
#include <sstream>
#include <exception>
#include <string>


#include <cstring>
#include <cstdlib>

#include <jlib/sys/sys.hh>

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

namespace jlib {
    namespace sys {

        template< typename charT, typename traitT = std::char_traits<charT> >
        class basic_socketbuf : public std::basic_streambuf<charT,traitT> {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = "socket exception: "+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            typedef charT 					            char_type;
            typedef traitT 					            traits_type;
            typedef typename traits_type::int_type 		int_type;
            typedef typename traits_type::pos_type 		pos_type;
            typedef typename traits_type::off_type 		off_type;
            
            static const unsigned int BUF_SIZE = 1024;

            basic_socketbuf(const std::string& host, unsigned int port) {
                char_type* tmp;
                
                tmp = new char_type[BUF_SIZE];
                this->setg(tmp,tmp,tmp);
                
                tmp = new char_type[BUF_SIZE];
                this->setp(tmp,tmp+BUF_SIZE);
                
                //_M_mode = (std::ios_base::in | std::ios_base::out);
                
                m_eintr = false;
                open_socket(host,port);
            }

            virtual ~basic_socketbuf() {
                if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_socketbuf::~basic_socketbuf()"<<std::endl;
                close();

                delete [] this->eback();
                delete [] this->pbase();
            }

            virtual int_type underflow() {
                if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_socketbuf::underflow()"<<std::endl;

                m_eintr = false;
                int count = ::read(m_sock, this->eback(), BUF_SIZE);
                
                if(count < 0) {
                    if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                        std::cerr <<"exception in read(2) at jlib::sys::socketstream::underflow()"<<std::endl;
                    //this->setstate(std::ios_base::badbit);
                    if(errno == EINTR) {
                        m_eintr = true;
                    }
                    return traits_type::eof();
                    //throw exception("error reading");
                }                
                else if(count == 0) {
                    if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                        std::cerr <<"eof in read(2) at jlib::sys::socketstream::underflow()"<<std::endl;
                    return traits_type::eof();
                }
                else {
                    char_type* end = this->eback()+count;
                    this->setg(this->eback(), this->eback(), end);
                    
                    return traits_type::to_int_type(*this->gptr());
                }
            }

            virtual int_type overflow(int_type c=traits_type::eof()) {
                if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_socketbuf::overflow("<<c<<")"<<std::endl;
                if(this->pptr() >= this->epptr()) {
                    if(sync() == traits_type::eof()) {
                        return traits_type::eof();
                    }
                }
                
                *this->pptr() = c;
                this->pbump(1);
                return c;
            }

            virtual int_type sync() {
                if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_socketbuf::sync()"<<std::endl;
                // Where SO_NOSIGPIPE exists this is a no-op and the socket
                // option has already dealt with it; where it does not, this is
                // what keeps a dead peer from killing us.
                sigpipe_guard guard;

                int sofar = 0;
                int total = this->pptr() - this->pbase();
                int diff;
                int count;
                char_type* current = this->pbase();
                
                while( (diff=(total-sofar)) > 0 ) {
                    m_eintr = false;
                    count = write(m_sock, current, diff);

                    if(count == -1) {
                        if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                            std::cerr <<"exception in jlib::sys::socketstream::sync()"<<std::endl;
                        if(errno == EINTR) {
                            m_eintr = true;
                        }
                        // EPIPE arrives here now instead of as a signal.
                        return traits_type::eof();
                    }
                    sofar += count;
                    current += count;
                }
                
                this->setp(this->pbase(), this->pbase()+BUF_SIZE);
                return 0;                
            }

            virtual void close() {
                if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_socketbuf::close()"<<std::endl;
                if(m_sock != -1) {
                    ::close(m_sock);
                    m_sock = -1;
                }
            }

            bool interrupted() { return m_eintr; }

            int get_socket() { return m_sock; }
            
        protected:
            void open_socket(const std::string& host, unsigned int port) {
                m_host = host;
                m_port = port;

                struct sockaddr_in sa;
                struct hostent* hp;
                
                if( (hp=gethostbyname(host.c_str())) == NULL ) {
                    m_sock = -1;
                    if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                        std::cerr <<"throwing exception from jlib::sys::socketstream::open_socket()"<<std::endl
                                  << "error in gethostbyname("<<host<<")"<<std::endl;
                    throw exception("error resolving "+host);
                }
                
                std::memset(&sa,0,sizeof(sa));
                memcpy(reinterpret_cast<char*>(&sa.sin_addr),hp->h_addr,hp->h_length);
                sa.sin_family = hp->h_addrtype;
                sa.sin_port = htons((u_short)port);
                
                if( (m_sock=socket(hp->h_addrtype,SOCK_STREAM,0)) < 0 ) {
                    if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                        std::cerr <<"throwing exception from jlib::sys::socketstream::open_socket()"<<std::endl
                                  << "error in socket()"<<std::endl;
                    throw exception("error in socket()");
                }
                
                m_eintr = false;
                if(connect(m_sock,reinterpret_cast<struct sockaddr*>(&sa),sizeof(sa)) < 0) {
                    if(errno == EINTR) {
                        m_eintr = true;
                    }
                    ::close(m_sock);
                    if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                        std::cerr <<"throwing exception from jlib::sys::socketstream::open_socket()"<<std::endl
                                  <<"error in connect()"<<std::endl;
                    std::ostringstream o; o << port;
                    throw exception("error connecting to " + host + ":" + o.str());
                }
                
                // Before anything is written: a write to a peer that has gone
                // away raises SIGPIPE, whose default action is to kill the
                // process outright, so the error checking below never runs.
                nosigpipe(m_sock);

                if(fcntl(m_sock, F_SETFD, 1) == -1) {
                    if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                        std::cerr <<"throwing exception from jlib::sys::socketstream::open_socket()"<<std::endl
                                  <<"error calling fcntl(sock, F_SETFD, 1)"<<std::endl;
                    throw exception("error calling fcntl(sock, F_SETFD, 1)");
                }
                //free(hp);
            }

            std::string m_host;
            unsigned int m_port;
            int m_sock;
            bool m_eintr;
        };
        
        template<typename charT, typename traitT=std::char_traits<charT> >
        class basic_socketstream : public std::basic_iostream<charT,traitT> {
        public:
            basic_socketstream() 
                : std::basic_iostream<charT,traitT>(NULL)
            {
                m_buf = 0;
                //exceptions(std::ios_base::badbit);
            }

            basic_socketstream(const std::string& host, unsigned int port)
                : std::basic_iostream<charT,traitT>(NULL)
            {
                m_buf = 0;
                //exceptions(std::ios_base::badbit);
                m_buf=new basic_socketbuf<charT,traitT>(host,port);
                this->init(m_buf);
            }

            virtual ~basic_socketstream() {
                if(m_buf != 0)
                    delete m_buf;
            }
            
            void open(const std::string& host, unsigned int port) {
                if(m_buf != 0)
                    delete m_buf;
                m_buf=new basic_socketbuf<charT,traitT>(host,port);
                this->init(m_buf);
            }

            void close() {
                m_buf->close();
            }

            bool interrupted() { return m_buf->interrupted(); }

            int get_socket() { return m_buf->get_socket(); }
            
        protected:
            basic_socketbuf<charT,traitT>* m_buf;
        };
    
        typedef basic_socketstream< char, std::char_traits<char> > socketstream;
        
    }
}


#endif // JLIB_SYS_SOCKETSTREAM_HH
