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

#ifndef JLIB_SYS_PSTREAM_HH
#define JLIB_SYS_PSTREAM_HH


#include <cstring>
#include <iostream>
#include <exception>
#include <string>


#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

namespace jlib {
    namespace sys {

        /**
         * A streambuf over popen(3).
         *
         * Every call into the base is written `this->eback()` rather than
         * `eback()`, and has to be: the base depends on the template
         * parameters, so an unqualified name is looked up when the template is
         * defined, at which point it does not exist.  Not a style choice --
         * without it this header does not compile at all, which nothing
         * noticed for as long as nothing instantiated it.
         */
        template< typename charT, typename traitT = std::char_traits<charT> >
        class basic_procbuf : public std::basic_streambuf<charT,traitT> {
        public:
            typedef charT 					            char_type;
            typedef traitT 					            traits_type;
            typedef typename traits_type::int_type 		int_type;
            typedef typename traits_type::pos_type 		pos_type;
            typedef typename traits_type::off_type 		off_type;
            
            static const unsigned int BUF_SIZE = 1024;

            basic_procbuf(const std::string& cmd, std::ios_base::openmode mode) {
                char_type* tmp;
                
                tmp = new char_type[BUF_SIZE];
                this->setg(tmp,tmp,tmp);
                
                tmp = new char_type[BUF_SIZE];
                this->setp(tmp,tmp+BUF_SIZE);
                
                //_M_mode = (std::ios_base::in | std::ios_base::out);
                
                m_eintr = false;
                open_process(cmd,mode);
            }

            virtual ~basic_procbuf() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_procbuf::~basic_procbuf()"<<std::endl;
                close();

                delete [] this->eback();
                delete [] this->pbase();
            }

            virtual int_type underflow() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_procbuf::underflow()"<<std::endl;

                m_eintr = false;
                int count = ::read(m_pd, this->eback(), BUF_SIZE);
                
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
                    char_type* end = this->eback()+count;
                    this->setg(this->eback(), this->eback(), end);
                    
                    return *this->gptr();
                }
            }

            virtual int_type overflow(int_type c=traits_type::eof()) {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_procbuf::overflow("<<c<<")"<<std::endl;
                if(this->pptr() >= this->epptr()) {
                    if(sync() == -1) {
                        return traits_type::eof();
                    }
                }
                
                *this->pptr() = c;
                this->pbump(1);
                return c;
            }

            virtual int_type sync() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_procbuf::sync()"<<std::endl;
                int sofar = 0;
                int total = this->pptr() - this->pbase();
                int diff;
                int count;
                char_type* current = this->pbase();
                
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
                
                this->setp(this->pbase(), this->pbase()+BUF_SIZE);
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
            void open_process(const std::string& cmd, std::ios_base::openmode mode) {
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

            basic_pstream(const std::string& cmd, std::ios_base::openmode mode)
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
            
            void open(const std::string& cmd, std::ios_base::openmode mode) {
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
