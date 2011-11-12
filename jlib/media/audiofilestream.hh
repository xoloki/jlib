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

#ifndef JLIB_MEDIA_AUDIOFILESTREAM_HH
#define JLIB_MEDIA_AUDIOFILESTREAM_HH

#include <iostream>
#include <exception>
#include <string>
#include <cstring>
#include <cmath>

#include <bits/char_traits.h>

#include <errno.h>
#include <sys/soundcard.h>
#include <sys/types.h>
#include <netinet/in.h>

#include <jlib/media/datastream.hh>
#include <jlib/media/AudioFile.hh>
#include <jlib/util/util.hh>

namespace jlib {
    namespace media {
        
        //typedef enum { A1=110, A2=220, A3=440, A4=880 } audiofile;
        
        template< typename charT, typename traitT = std::char_traits<charT> >
        class basic_audiofilebuf : public basic_databuf<charT,traitT> {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = std::string("jlib::media::basic_audiofilebuf::exception")+(msg==""?"":": ")+msg;
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
            
            /**
             * default ctor
             */
            basic_audiofilebuf(); 
            /**
             * parse audiofile string like A#1
             */
            basic_audiofilebuf(AudioFile* file);
            
            //virtual int_type underflow();
            //virtual int_type overflow(int_type c=traits_type::eof());
            virtual int_type sync();

            AudioFile* get_audiofile() const; 
            void set_audiofile(AudioFile* file);

        protected:
            AudioFile* m_audiofile;
        };
        
        template<typename charT, typename traitT=std::char_traits<charT> >
        class basic_audiofilestream : public basic_datastream<charT,traitT> {
        public:
            basic_audiofilestream();
            basic_audiofilestream(AudioFile* file);
            
            AudioFile* get_audiofile() const; 
            void set_audiofile(AudioFile* file);

        protected:
        };
        
        typedef basic_audiofilestream<char> audiofilestream;

        
        template< typename charT, typename traitT >
        inline
        basic_audiofilebuf<charT,traitT>::basic_audiofilebuf() 
            : basic_databuf<charT,traitT>()
        {
        }
        
        template< typename charT, typename traitT >
        inline
        basic_audiofilebuf<charT,traitT>::basic_audiofilebuf(AudioFile* file)
            : basic_databuf<charT,traitT>()
        {
            set_audiofile(file);
        }
        
        template< typename charT, typename traitT >
        inline
        AudioFile* basic_audiofilebuf<charT,traitT>::get_audiofile() const {
            return m_audiofile;
        }

        template< typename charT, typename traitT >
        inline
        void basic_audiofilebuf<charT,traitT>::set_audiofile(AudioFile* audiofile) {
            m_audiofile = audiofile;
            
            this->set_data(m_audiofile->get_pcm());
        }

        template< typename charT, typename traitT >
        inline
        typename basic_audiofilebuf<charT,traitT>::int_type 
        basic_audiofilebuf<charT,traitT>::sync() {
            return traits_type::eof();
        }

        template< typename charT, typename traitT >
        inline
        basic_audiofilestream<charT,traitT>::basic_audiofilestream() 
            : basic_datastream<charT,traitT>()
        {
            if(this->m_buf) delete this->m_buf;
            this->m_buf=new basic_audiofilebuf<charT,traitT>();
            this->init(this->m_buf);
        }
        
        template< typename charT, typename traitT >
        inline
        basic_audiofilestream<charT,traitT>::basic_audiofilestream(AudioFile* audiofile)
            : basic_datastream<charT,traitT>()
        {
            this->m_buf=new basic_audiofilebuf<charT,traitT>(audiofile);
            this->init(this->m_buf);
        }
        
        template< typename charT, typename traitT >
        inline
        AudioFile* 
        basic_audiofilestream<charT,traitT>::get_audiofile() const
        {
            if(!this->m_buf)
                throw basic_audiofilebuf<charT,traitT>::exception("m_buf == null");
            basic_audiofilebuf<charT,traitT>* buf = dynamic_cast< basic_audiofilebuf<charT,traitT>* >(this->m_buf);
            if(buf)
                return buf->get_audiofile();
            else
                throw basic_audiofilebuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        void
        basic_audiofilestream<charT,traitT>::set_audiofile(AudioFile* audiofile)
        {
            if(!this->m_buf)
                throw basic_audiofilebuf<charT,traitT>::exception("m_buf == null");
            basic_audiofilebuf<charT,traitT>* buf = dynamic_cast< basic_audiofilebuf<charT,traitT>* >(this->m_buf);
            if(buf)
                buf->set_audiofile(audiofile);
            else
                throw basic_audiofilebuf<charT,traitT>::exception("buf == null");
        }

    }
}


#endif // JLIB_MEDIA_STREAM_HH
