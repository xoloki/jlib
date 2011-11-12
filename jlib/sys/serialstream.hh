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
                exception(std::string msg = "") {
                    m_msg = "serial exception: "+msg;
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

            basic_serialbuf(std::string dev, std::ios_base::openmode mode);
            virtual ~basic_serialbuf();

            virtual int_type underflow();
            virtual int_type overflow(int_type c=traits_type::eof());
            virtual int_type sync();

            void close();

        protected:
            void open_serial(std::string dev, std::ios_base::openmode mode);

            std::string m_dev;
            unsigned int m_port;
            int m_sock;
        };
        
        template<class charT, class traitT=std::char_traits<charT> >
        class basic_serialstream : public std::basic_iostream<charT> {
        public:
            basic_serialstream();
            basic_serialstream(std::string dev, std::ios_base::openmode mode);

            void open(std::string dev, std::ios_base::openmode mode);
            void close();
        private:
            basic_serialbuf<charT,traitT>* m_buf;
        };
    
        typedef basic_serialstream< char, std::char_traits<char> > serialstream;
            

        
    }
}


#endif // JLIB_SYS_SERIALSTREAM_HH
