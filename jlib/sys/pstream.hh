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

#ifndef JLIB_SYS_PSTREAM_HH
#define JLIB_SYS_PSTREAM_HH


#include <iostream>
#include <exception>
#include <string>

#include <bits/char_traits.h>

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
        class basic_procbuf : public std::basic_streambuf<charT,traitT> {
        public:
            typedef charT 					            char_type;
            typedef traitT 					            traits_type;
            typedef typename traits_type::int_type 		int_type;
            typedef typename traits_type::pos_type 		pos_type;
            typedef typename traits_type::off_type 		off_type;
            
            static const unsigned int BUF_SIZE = 1024;

            basic_procbuf(std::string cmd, std::ios_base::openmode mode) {
                char_type* tmp;
                
                tmp = new char_type[BUF_SIZE];
                setg(tmp,tmp,tmp);
                
                tmp = new char_type[BUF_SIZE];
                setp(tmp,tmp+BUF_SIZE);
                
                //_M_mode = (std::ios_base::in | std::ios_base::out);
                
                m_eintr = false;
                open_process(cmd,mode);
            }

            virtual ~basic_procbuf() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_procbuf::~basic_procbuf()"<<std::endl;
                close();

                delete [] eback();
                delete [] pbase();
            }

            virtual int_type underflow() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_procbuf::underflow()"<<std::endl;

                m_eintr = false;
                int count = ::read(m_pd, eback(), BUF_SIZE);
                
                if(count < 0) {
                    if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                        std::cerr <<"exception in read(2) at jlib::sys::pstream::underflow()"<<std::endl;
                    //this->setstate(std::ios_base::badbit);
                    if(errno == EINTR) {
                        m_eintr = true;
                    }
                    return -1;
                    //throw exception("error reading");
                }                
                else if(count == 0) {
                    if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                        std::cerr <<"eof in read(2) at jlib::sys::pstream::underflow()"<<std::endl;
                    return traits_type::eof();
                }
                else {
                    char_type* end = eback()+count;
                    setg(eback(), eback(), end);
                    
                    return *gptr();
                }
            }

            virtual int_type overflow(int_type c=traits_type::eof()) {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_procbuf::overflow("<<c<<")"<<std::endl;
                if(pptr() >= epptr()) {
                    if(sync() == -1) {
                        return traits_type::eof();
                    }
                }
                
                *pptr() = c;
                pbump(1);
                return c;
            }

            virtual int_type sync() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_procbuf::sync()"<<std::endl;
                int sofar = 0;
                int total = pptr() - pbase();
                int diff;
                int count;
                char_type* current = pbase();
                
                while( (diff=(total-sofar)) > 0 ) {
                    m_eintr = false;
                    count = write(m_pd, current, diff);

                    if(count == -1) {
                        if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                            std::cerr <<"exception in jlib::sys::pstream::overflow()"<<std::endl;
                        //this->setstate(std::ios_base::badbit);
                        if(errno == EINTR) {
                            m_eintr = true;
                        }
                        return -1;
                        //throw exception("error writing");
                    }
                    sofar += count;
                    current += count;
                }
                
                setp(pbase(), pbase()+BUF_SIZE);
                return 0;                
            }

            virtual void close() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_procbuf::close()"<<std::endl;
                if(m_filep != 0) {
                    m_exitval = pclose(m_filep);
                    m_filep = 0;
                }
            }

            bool interrupted() { return m_eintr; }
            int exitval() { return m_exitval; }

        protected:
            void open_process(std::string cmd, std::ios_base::openmode mode) {
                m_cmd = cmd;
                m_mode = mode;
                m_filep = 0;
                if(mode == std::ios_base::in) {
                    m_filep = popen(cmd.c_str(), "r");
                }
                else if(mode == std::ios_base::out) {
                    m_filep = popen(cmd.c_str(), "w");
                }
                if(m_filep == NULL) {
                    std::cerr << "error opening process \""<<cmd<<"\": " 
                              << strerror(errno) << std::endl;
                    exit(1);
                }
                m_pd = fileno(m_filep);
            }

            std::string m_cmd;
            std::ios_base::openmode m_mode;
            int m_pd, m_exitval;
            bool m_eintr;
            FILE* m_filep;
        };
        
        template<typename charT, typename traitT=std::char_traits<charT> >
        class basic_pstream : public std::basic_iostream<charT,traitT> {
        public:
            basic_pstream() 
                : std::basic_iostream<charT,traitT>(NULL)
            {
                m_buf = 0;
            }

            basic_pstream(std::string cmd, std::ios_base::openmode mode)
                : std::basic_iostream<charT,traitT>(NULL)
            {
                m_buf = 0;
                m_buf=new basic_procbuf<charT,traitT>(cmd,mode);
                this->init(m_buf);
            }

            virtual ~basic_pstream() {
                if(m_buf != 0)
                    delete m_buf;
            }
            
            void open(std::string cmd, std::ios_base::openmode mode) {
                if(m_buf != 0)
                    delete m_buf;
                m_buf=new basic_procbuf<charT,traitT>(cmd,mode);
                this->init(m_buf);
            }

            void close() {
                m_buf->close();
            }

            int exitval() {
                return m_buf->exitval();
            }

            bool interrupted() { return m_buf->interrupted(); }

        protected:
            basic_procbuf<charT,traitT>* m_buf;
        };
    
        typedef basic_procbuf< char, std::char_traits<char> > procbuf;
        typedef basic_pstream< char, std::char_traits<char> > pstream;
        
    }
}

#endif // JLIB_SYS_PSTREAM_HH
