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

#ifndef JLIB_MEDIA_WAVSTREAM_HH
#define JLIB_MEDIA_WAVSTREAM_HH

#include <iostream>
#include <exception>
#include <string>
#include <cstring>

#include <bits/char_traits.h>

#include <errno.h>

#include <jlib/media/stream.hh>

namespace jlib {
    namespace media {
        
        template< typename charT, typename traitT = std::char_traits<charT> >
        class basic_wavbuf : public basic_streambuf<charT,traitT> {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = std::string("jlib::media::basic_wavbuf::exception")+(msg==""?"":": ")+msg;
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
            
            basic_wavbuf();
            basic_wavbuf(std::string file);
            
            virtual int_type underflow();
            //virtual int_type overflow(int_type c=traits_type::eof());
            virtual int_type sync();

            std::string get_file() const; 
            void set_file(std::string s);

        protected:

            std::string create_riff_header();
            std::string create_format_chunk();
            std::string create_data_chunk();

            std::string m_file;
            std::string::size_type m_p;
        };
        
        template<typename charT, typename traitT=std::char_traits<charT> >
        class basic_wavstream : public basic_stream<charT,traitT> {
        public:
            basic_wavstream();
            basic_wavstream(std::string file);
            
            std::string get_file() const; 
            void set_file(std::string s);

        protected:
        };
        
        typedef basic_wavstream<char> wavstream;
        
        
        template< typename charT, typename traitT >
        inline
        basic_wavbuf<charT,traitT>::basic_wavbuf() 
            : basic_streambuf<charT,traitT>()
        {
            m_p = 0;
            set_file("");
        }
        
        template< typename charT, typename traitT >
        inline
        basic_wavbuf<charT,traitT>::basic_wavbuf(std::string file) 
            : basic_streambuf<charT,traitT>()
        {
            m_p = 0;
            set_file(file);
        }
        
        /*
        template< typename charT, typename traitT >
        inline
        basic_wavbuf<charT,traitT>::~basic_wavbuf() {
            
        }
        */
        
        template< typename charT, typename traitT >
        inline
        std::string basic_wavbuf<charT,traitT>::get_file() const {
            return m_file;
        }

        template< typename charT, typename traitT >
        inline
        void basic_wavbuf<charT,traitT>::set_file(std::string file) {
            m_file = file;
            set_length(m_file.length());
        }

        template< typename charT, typename traitT >
        inline
        typename basic_wavbuf<charT,traitT>::int_type 
        basic_wavbuf<charT,traitT>::underflow() {
            if(getenv("JLIB_MEDIA_STREAM_DEBUG"))
                std::cerr << "enter jlib::media::basic_wavbuf<charT,traitT>::underflow()"<<std::endl;
            
            if(m_wav.length() == 0) {
                if(getenv("JLIB_MEDIA_STREAM_DEBUG")) {
                    std::cerr << "\tm_wav.length() == 0, returning eof"<<std::endl;
                    std::cerr << "leave jlib::media::basic_wavbuf<charT,traitT>::underflow()"<<std::endl;
                }
                return traits_type::eof();
            }

            if(m_p == m_wav.length()) {
                if(getenv("JLIB_MEDIA_STREAM_DEBUG")) {
                    std::cerr << "\tm_p == m_wav.length(), returning eof"<<std::endl;
                    std::cerr << "leave jlib::media::basic_wavbuf<charT,traitT>::underflow()"<<std::endl;
                }
                return traits_type::eof();
            }
            //m_p = 0;

            if(m_p > m_wav.length())
                throw exception("m_p > m_wav.length() in underflow()");

            int count = (((m_p+BUF_SIZE) > m_wav.length()) ? (m_wav.length()-m_p) : (BUF_SIZE));
            if(getenv("JLIB_MEDIA_STREAM_DEBUG")) {
                std::cerr << "\tm_p = "<<m_p<<std::endl;
                std::cerr << "\tcount = "<<count<<std::endl;
                std::cerr << "\tm_wav.length() = "<<m_wav.length()<<std::endl;
                std::cerr << "\ttraits_type::eof() = "<<traits_type::eof()<<std::endl;
            }
            memcpy(eback(),m_wav.wav()+m_p,count);

            char_type* end = eback()+count;
            setg(eback(), eback(), end);
            
            m_p += count;

            if(getenv("JLIB_MEDIA_STREAM_DEBUG"))
                std::cerr << "leave jlib::media::basic_wavbuf<charT,traitT>::underflow(): return "
                          <<(int)*gptr() << std::endl;
            return traits_type::to_int_type(*gptr());
        }

        template< typename charT, typename traitT >
        inline
        typename basic_wavbuf<charT,traitT>::int_type 
        basic_wavbuf<charT,traitT>::sync() {
            if(getenv("JLIB_MEDIA_STREAM_DEBUG"))
                std::cerr << "basic_wavbuf<charT,traitT>::sync()"<<std::endl;

            m_wav.append(pbase(),pptr() - pbase());
            setp(pbase(), pbase()+BUF_SIZE);

            return 0;                
        }

        template< typename charT, typename traitT >
        inline
        std::string basic_wavbuf<charT,traitT>::create_riff_header() {
            std::string chunk(GROUP_ID_SIZE+RIFF_TYPE_SIZE, '\0');

            chunk.replace(0,GROUP_ID_SIZE,"RIFF\0\0\0\0");
            chunk.replace(RIFF_TYPE_OFFSET,RIFF_TYPE_SIZE,"WAVE");
            
            return chunk;
        }

        template< typename charT, typename traitT >
        inline
        std::string basic_wavbuf<charT,traitT>::create_format_chunk() {
            std::string chunk(CHUNK_ID_SIZE+CHUNK_SIZE_SIZE+FMT_CHUNK_SIZE, '\0');
            int base = CHUNK_ID_SIZE+CHUNK_SIZE_SIZE;

            chunk.replace(0,CHUNK_ID_SIZE,CHUNK_ID_FORMAT);
            util::set<int>(chunk,FMT_CHUNK_SIZE,CHUNK_SIZE_OFFSET);

            util::set<short>(chunk,get_format_tag(),base+FORMAT_TAG_OFFSET);
            util::set<u_short>(chunk,get_bits_per_sample(),base+BITS_PER_SAMPLE_OFFSET);
            util::set<u_long>(chunk,get_samples_per_sec(),base+SAMPLES_PER_SEC_OFFSET);
            util::set<u_short>(chunk,get_channels(),base+CHANNELS_OFFSET);
   
            return chunk;
        }

        template< typename charT, typename traitT >
        inline
        std::string basic_wavbuf<charT,traitT>::create_data_chunk() {
            std::string chunk(CHUNK_ID_SIZE+CHUNK_SIZE_SIZE, '\0');
            
            chunk.replace(0,CHUNK_ID_SIZE,CHUNK_ID_DATA);
            util::set<u_int>(chunk,get_pcm().length(),CHUNK_SIZE_OFFSET);

            chunk.append(get_pcm());

            return chunk;
        }












        template< typename charT, typename traitT >
        inline
        basic_wavstream<charT,traitT>::basic_wavstream() 
            : basic_stream<charT,traitT>()
        {
            m_buf=new basic_wavbuf<charT,traitT>();
            this->init(m_buf);
        }
        
        template< typename charT, typename traitT >
        inline
        basic_wavstream<charT,traitT>::basic_wavstream(std::string wav) 
            : basic_stream<charT,traitT>()
        {
            m_buf=new basic_wavbuf<charT,traitT>(wav);
            this->init(m_buf);
        }
        
        template< typename charT, typename traitT >
        inline
        std::string
        basic_wavstream<charT,traitT>::get_wav() const
        {
            if(!m_buf)
                throw basic_wavbuf<charT,traitT>::exception("m_buf == null");
            basic_wavbuf<charT,traitT>* buf = dynamic_cast< basic_wavbuf<charT,traitT>* >(m_buf);
            if(buf)
                return buf->get_wav();
            else
                throw basic_wavbuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        void
        basic_wavstream<charT,traitT>::set_wav(std::string wav) 
        {
            if(!m_buf)
                throw basic_wavbuf<charT,traitT>::exception("m_buf == null");
            basic_wavbuf<charT,traitT>* buf = dynamic_cast< basic_wavbuf<charT,traitT>* >(m_buf);
            if(buf)
                buf->set_wav(wav);
            else
                throw basic_wavbuf<charT,traitT>::exception("buf == null");
        }

    }
}


#endif // JLIB_MEDIA_STREAM_HH
