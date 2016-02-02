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

#ifndef JLIB_SYS_SOCKETSTREAM_HH
#define JLIB_SYS_SOCKETSTREAM_HH

#include <iostream>
#include <sstream>
#include <exception>
#include <string>

#include <bits/char_traits.h>

#include <cstring>
#include <cstdlib>

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
                exception(std::string msg = "") {
                    m_msg = "socket exception: "+msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            typedef charT 					            char_type;
            typedef traitT 					            traits_type;
            typedef typename traits_type::int_type 		int_type;
            typedef typename traits_type::pos_type 		pos_type;
            typedef typename traits_type::off_type 		off_type;
            
            static const unsigned int BUF_SIZE = 1024;

            basic_socketbuf(std::string host, unsigned int port) {
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

        protected:
            void open_socket(std::string host, unsigned int port) {
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

            basic_socketstream(std::string host, unsigned int port)
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
            
            void open(std::string host, unsigned int port) {
                if(m_buf != 0)
                    delete m_buf;
                m_buf=new basic_socketbuf<charT,traitT>(host,port);
                this->init(m_buf);
            }

            void close() {
                m_buf->close();
            }

            bool interrupted() { return m_buf->interrupted(); }

        protected:
            basic_socketbuf<charT,traitT>* m_buf;
        };
    
        typedef basic_socketstream< char, std::char_traits<char> > socketstream;
        
    }
}


#endif // JLIB_SYS_SOCKETSTREAM_HH
