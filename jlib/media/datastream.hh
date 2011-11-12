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

#ifndef JLIB_MEDIA_DATASTREAM_HH
#define JLIB_MEDIA_DATASTREAM_HH

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
        class basic_databuf : public basic_streambuf<charT,traitT> {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = std::string("jlib::media::basic_databuf::exception")+(msg==""?"":": ")+msg;
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
            
            basic_databuf();
            basic_databuf(std::string data);
            
            virtual int_type underflow();
            //virtual int_type overflow(int_type c=traits_type::eof());
            virtual int_type sync();

            virtual pos_type seekoff(off_type, std::ios_base::seekdir,
                                     std::ios_base::openmode = std::ios_base::in | std::ios_base::out);

            virtual pos_type seekpos(pos_type, 
                                     std::ios_base::openmode = std::ios_base::in | std::ios_base::out);

            std::string get_data() const; 
            void set_data(std::string s);

        protected:
            std::string m_data;
        };
        
        template<typename charT, typename traitT=std::char_traits<charT> >
        class basic_datastream : public basic_stream<charT,traitT> {
        public:
            basic_datastream();
            basic_datastream(std::string data);
            
            std::string get_data() const; 
            void set_data(std::string s);

            static basic_datastream<charT,traitT>* create_scaled(basic_stream<charT,traitT>* s);

        protected:
        };
        
        typedef basic_datastream<char> datastream;
        
        
        template< typename charT, typename traitT >
        inline
        basic_databuf<charT,traitT>::basic_databuf() 
            : basic_streambuf<charT,traitT>()
        {
            set_data("");
        }
        
        template< typename charT, typename traitT >
        inline
        basic_databuf<charT,traitT>::basic_databuf(std::string data) 
            : basic_streambuf<charT,traitT>()
        {
            set_data(data);
        }
        
        /*
        template< typename charT, typename traitT >
        inline
        basic_databuf<charT,traitT>::~basic_databuf() {
            
        }
        */
        
        template< typename charT, typename traitT >
        inline
        std::string basic_databuf<charT,traitT>::get_data() const {
            return m_data;
        }

        template< typename charT, typename traitT >
        inline
        void basic_databuf<charT,traitT>::set_data(std::string data) {
            m_data = data;
            this->set_length(m_data.length());
        }

        template< typename charT, typename traitT >
        inline
        typename basic_databuf<charT,traitT>::int_type 
        basic_databuf<charT,traitT>::underflow() {
            if(getenv("JLIB_MEDIA_STREAM_DEBUG"))
                std::cerr << "enter jlib::media::basic_databuf<charT,traitT>::underflow()"<<std::endl;
            
            if(m_data.length() == 0) {
                if(getenv("JLIB_MEDIA_STREAM_DEBUG")) {
                    std::cerr << "\tm_data.length() == 0, returning eof"<<std::endl;
                    std::cerr << "leave jlib::media::basic_databuf<charT,traitT>::underflow()"<<std::endl;
                }
                return traits_type::eof();
            }

            if(this->m_pos == this->m_length) {
                if(getenv("JLIB_MEDIA_STREAM_DEBUG")) {
                    std::cerr << "\tm_p == m_length, returning eof"<<std::endl;
                    std::cerr << "leave jlib::media::basic_databuf<charT,traitT>::underflow()"<<std::endl;
                }
                return traits_type::eof();
            }

            if(this->m_pos > this->m_length)
                throw exception("m_pos > m_length in underflow()");

            seekpos(this->m_pos, std::ios_base::in);

            if(getenv("JLIB_MEDIA_STREAM_DEBUG"))
                std::cerr << "leave jlib::media::basic_databuf<charT,traitT>::underflow(): return "
                          <<(int)*this->gptr() << std::endl;
            return traits_type::to_int_type(*this->gptr());
        }

        template< typename charT, typename traitT >
        inline
        typename basic_databuf<charT,traitT>::int_type 
        basic_databuf<charT,traitT>::sync() {
            if(getenv("JLIB_MEDIA_STREAM_DEBUG"))
                std::cerr << "basic_databuf<charT,traitT>::sync()"<<std::endl;

            this->m_data.append(this->pbase(), this->pptr() - this->pbase());
            setp(this->pbase(), this->pbase() + BUF_SIZE);

            return 0;                
        }


        template< typename charT, typename traitT >
        inline
        typename basic_databuf<charT,traitT>::pos_type 
        basic_databuf<charT,traitT>::seekoff(off_type o, std::ios_base::seekdir s,
                                             std::ios_base::openmode m)
        {
            if(getenv("JLIB_MEDIA_DATASTREAM_DEBUG")) {
                std::cerr << "basic_databuf<charT,traitT>::seekoff("<<o<<",";
                switch(s) {
                case std::ios_base::beg:
                    std::cerr << "std::ios_base::beg,";
                    break;
                case std::ios_base::cur:
                    std::cerr << "std::ios_base::cur,";
                    break;
                case std::ios_base::end:
                    std::cerr << "std::ios_base::end,";
                    break;
                }
                switch(m) {
                case std::ios_base::in:
                    std::cerr << "std::ios_base::in)";
                    break;
                case std::ios_base::out:
                    std::cerr << "std::ios_base::out)";
                    break;
                }

                std::cerr << std::endl;
            }

            pos_type pos;
            off_type ptr_plus;
            off_type ptr_minus;
            switch(s) {
            case std::ios_base::beg:
                pos = static_cast<pos_type>(o);
                break;
            case std::ios_base::cur:
                ptr_plus  = ( (this->gptr() && this->egptr() && this->egptr() > this->gptr()) ? (this->egptr() - this->gptr()) : 0 );
                ptr_minus = ( (this->gptr() && this->eback() && this->gptr() > this->eback()) ? (this->gptr() - this->eback()) : 0 );
                pos = this->m_pos - static_cast<pos_type>(ptr_plus) + static_cast<pos_type>(o);

                if(ptr_plus &&
                   ( (o == static_cast<pos_type>(0)) ||
                     (o > static_cast<pos_type>(0) && o        <= static_cast<pos_type>(ptr_plus)) ||
                     (o < static_cast<pos_type>(0) && ((-1)*o) <= static_cast<pos_type>(ptr_minus)))) {
                    // no need to resync
                    this->gbump(static_cast<int>(o));
                    return (pos);
                    }
                break;
            case std::ios_base::end:
                pos = static_cast<pos_type>(o) + this->m_length;
                break;
            }

            return this->seekpos(pos,m);
        }

        template< typename charT, typename traitT >
        inline
        typename basic_databuf<charT,traitT>::pos_type 
        basic_databuf<charT,traitT>::seekpos(pos_type p, 
                                             std::ios_base::openmode m)
        {
            if(getenv("JLIB_MEDIA_DATASTREAM_DEBUG")) {
                std::cerr << "basic_databuf<charT,traitT>::seekpos("<<p<<",";
                switch(m) {
                case std::ios_base::in:
                    std::cerr << "std::ios_base::in)";
                    break;
                case std::ios_base::out:
                    std::cerr << "std::ios_base::out)";
                    break;
                }

                std::cerr << std::endl;
            }

            /*
            int count = (((static_cast<int>(p)+BUF_SIZE) > m_length) ? 
                         static_cast<int>(m_length-p) : (BUF_SIZE));
            memcpy(eback(),m_data.data()+p,count);

            char_type* end = eback()+count;
            setg(eback(), eback(), end);
            
            m_pos = p + static_cast<pos_type>(count);
            */
            
            if(p < m_data.length()) {
                char_type* c = const_cast<char_type*>(m_data.data())+static_cast<int>(p);
                char_type* e = const_cast<char_type*>(m_data.data())+static_cast<int>(m_data.length());
                setg(c, c, e);
                this->m_pos = static_cast<pos_type>(m_data.length());
            }
            else {
                return traits_type::eof();
            }

            return p;
        }


        template< typename charT, typename traitT >
        inline
        basic_datastream<charT,traitT>::basic_datastream() 
            : basic_stream<charT,traitT>()
        {
            this->m_buf=new basic_databuf<charT,traitT>();
            this->init(this->m_buf);
        }
        
        template< typename charT, typename traitT >
        inline
        basic_datastream<charT,traitT>::basic_datastream(std::string data) 
            : basic_stream<charT,traitT>()
        {
            this->m_buf=new basic_databuf<charT,traitT>(data);
            this->init(this->m_buf);
        }
        
        template< typename charT, typename traitT >
        inline
        std::string
        basic_datastream<charT,traitT>::get_data() const
        {
            if(!this->m_buf)
                throw basic_databuf<charT,traitT>::exception("this->m_buf == null");
            basic_databuf<charT,traitT>* buf = dynamic_cast< basic_databuf<charT,traitT>* >(this->m_buf);
            if(buf)
                return buf->get_data();
            else
                throw basic_databuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        void
        basic_datastream<charT,traitT>::set_data(std::string data) 
        {
            if(!this->m_buf)
                throw basic_databuf<charT,traitT>::exception("this->m_buf == null");
            basic_databuf<charT,traitT>* buf = dynamic_cast< basic_databuf<charT,traitT>* >(this->m_buf);
            if(buf)
                buf->set_data(data);
            else
                throw typename basic_databuf<charT,traitT>::exception("buf == null");
        }

        template< typename charT, typename traitT >
        inline
        basic_datastream<charT,traitT>* 
        basic_datastream<charT,traitT>::create_scaled(basic_stream<charT,traitT>* s) {
            int n = s->get_length() / (s->get_bits_per_sample()/8);
            Type::scaled samples[n];
            memset(samples,0,n*sizeof(Type::scaled));
            s->rewind();

            for(int i=0;i<n;i++) {
                samples[i] = s->get_scaled();
            }
            

            basic_datastream<charT,traitT>* r = 
                new basic_datastream<charT,traitT>(std::string(reinterpret_cast<char*>(samples),
                                                               n*sizeof(Type::scaled)));
            r->set(*s);
            r->set_format(Type::PCM_FLOAT32);

            return r;
        }

    }
}


#endif // JLIB_MEDIA_STREAM_HH
