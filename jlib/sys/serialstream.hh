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

#ifndef JLIB_SYS_SERIALSTREAM_HH
#define JLIB_SYS_SERIALSTREAM_HH

#include <fstream>
#include <exception>
#include <string>

namespace jlib {
    namespace sys {


        template< typename charT, typename traitT = std::char_traits<charT> >
        class basic_serialbuf : public std::basic_streambuf<charT,traitT> {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = "serial exception: "+msg;
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

            basic_serialbuf(const std::string& dev, std::ios_base::openmode mode);
            virtual ~basic_serialbuf();

            virtual int_type underflow();
            virtual int_type overflow(int_type c=traits_type::eof());
            virtual int_type sync();

            void close();

        protected:
            void open_serial(const std::string& dev, std::ios_base::openmode mode);

            std::string m_dev;
            unsigned int m_port;
            int m_sock;
        };
        
        template<class charT, class traitT=std::char_traits<charT> >
        class basic_serialstream : public std::basic_iostream<charT> {
        public:
            basic_serialstream();
            basic_serialstream(const std::string& dev, std::ios_base::openmode mode);

            void open(const std::string& dev, std::ios_base::openmode mode);
            void close();
        private:
            basic_serialbuf<charT,traitT>* m_buf;
        };
    
        typedef basic_serialstream< char, std::char_traits<char> > serialstream;
            

        
    }
}


#endif // JLIB_SYS_SERIALSTREAM_HH
