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

#ifndef JLIB_MEDIA_STREAM_HH
#define JLIB_MEDIA_STREAM_HH

#include <jlib/media/Type.hh>

#include <jlib/sys/sys.hh>
#include <jlib/util/util.hh>

#include <iostream>
#include <exception>
#include <string>
#include <memory>
#include <vector>
#include <list>

#include <cstdlib>


#include <errno.h>

namespace jlib {
    namespace media {
        
        template< typename charT, typename traitT = std::char_traits<charT> >
        class basic_streambuf : public std::basic_streambuf<charT,traitT> {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = std::string("jlib::media::basic_streambuf::exception")+(msg==""?"":": ")+msg;
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
            
            class plugin {
            public:
                virtual void run(basic_streambuf<charT,traitT>* s, 
                                 char_type* buf, 
                                 pos_type p, 
                                 pos_type n) = 0;
            };

            static const unsigned int BUF_SIZE = 1024;
            
            basic_streambuf();
            virtual ~basic_streambuf();

            /**
             * Not copyable.
             *
             * std::basic_streambuf leaves its copy operations protected rather
             * than deleted, so a derived class gets implicit ones -- and both
             * compiled here and double-freed, since the two buffers were raw
             * pointers deleted in the destructor.  The vectors below fix the
             * ownership, but a copy would still be wrong: the base copies the
             * get and put area pointers, so the copy's stream positions would
             * address the original's storage.  A streambuf is not a value.
             *
             * basic_stream needs no such declaration -- std::basic_iostream
             * deletes its copy operations, so the whole stream layer is
             * already uncopyable by inheritance.
             */
            basic_streambuf(const basic_streambuf&) = delete;
            basic_streambuf& operator=(const basic_streambuf&) = delete;
            
            //virtual int_type underflow();
            virtual int_type overflow(int_type c=traits_type::eof());
            //virtual int_type sync();
            //virtual void close();

            virtual int get_bits_per_sample() const; 
            virtual int get_channels() const; 
            virtual int get_samples_per_sec() const; 
            virtual int get_format() const; 

            pos_type get_length() const; 

            virtual void set_bits_per_sample(int s); 
            virtual void set_channels(int s); 
            virtual void set_samples_per_sec(int s); 
            virtual void set_format(int s); 

            void set_length(pos_type s); 

            bool is_interrupted() const;

            virtual void close();
            
        protected:
            int m_bits_per_sample, m_channels, m_samples_per_sec, m_format;
            bool m_eintr;
            pos_type m_length;
            pos_type m_pos;
            std::list<plugin*> m_plugins;
            // Vectors rather than new[], so the buffer owns them and nothing
            // has to remember to free them.  Sized once in the constructor and
            // never resized, so the pointers handed to setg and setp stay
            // valid for the life of the object.
            std::vector<char_type> m_get_buf;
            std::vector<char_type> m_put_buf;
        };
        
        template<typename charT, typename traitT=std::char_traits<charT> >
        class basic_stream : public std::basic_iostream<charT,traitT> {
        public:
            typedef typename basic_streambuf<charT,traitT>::pos_type pos_type;
            typedef typename basic_streambuf<charT,traitT>::off_type off_type;

            basic_stream();
            virtual ~basic_stream();
            
            int get_bits_per_sample() const; 
            int get_channels() const; 
            int get_samples_per_sec() const; 
            int get_format() const; 

            pos_type get_length() const; 

            void set_bits_per_sample(int s); 
            void set_channels(int s); 
            void set_samples_per_sec(int s); 
            void set_format(int s); 

            void set_length(pos_type s); 

            bool is_interrupted() const;

            virtual void close();
            virtual void rewind();

            template<class T>
            void set(const T& t) {
                set_bits_per_sample(t.get_bits_per_sample());
                set_channels(t.get_channels());
                set_samples_per_sec(t.get_samples_per_sec());
                set_format(t.get_format());
            }

            Type::scaled get_scaled() {
                char data[16];
                //std::string data;

                switch(get_format()) {
                case Type::PCM_U8:
                    sys::read(*this,data,sizeof(Type::sample<Type::PCM_U8>::buf));
                    return Type::sample<Type::PCM_U8>::scale(util::get<Type::sample<Type::PCM_U8>::buf>(data));

                case Type::PCM_S8:
                    sys::read(*this,data,sizeof(Type::sample<Type::PCM_S8>::buf));
                    return Type::sample<Type::PCM_S8>::scale(util::get<Type::sample<Type::PCM_S8>::buf>(data));

                case Type::PCM_S16_LE:
                    sys::read(*this,data,sizeof(Type::sample<Type::PCM_S16_LE>::buf));
                    return Type::sample<Type::PCM_S16_LE>::scale(util::get<Type::sample<Type::PCM_S16_LE>::buf>(data));

                case Type::PCM_S16_BE:
                    sys::read(*this,data,sizeof(Type::sample<Type::PCM_S16_BE>::buf));
                    return Type::sample<Type::PCM_S16_BE>::scale(util::get<Type::sample<Type::PCM_S16_BE>::buf>(data));

                case Type::PCM_U16_LE:
                    sys::read(*this,data,sizeof(Type::sample<Type::PCM_U16_LE>::buf));
                    return Type::sample<Type::PCM_U16_LE>::scale(util::get<Type::sample<Type::PCM_U16_LE>::buf>(data));

                case Type::PCM_U16_BE:
                    sys::read(*this,data,sizeof(Type::sample<Type::PCM_U16_BE>::buf));
                    return Type::sample<Type::PCM_U16_BE>::scale(util::get<Type::sample<Type::PCM_U16_BE>::buf>(data));

                case Type::PCM_MPEG:
                    sys::read(*this,data,sizeof(Type::sample<Type::PCM_MPEG>::buf));
                    return Type::sample<Type::PCM_MPEG>::scale(util::get<Type::sample<Type::PCM_MPEG>::buf>(data));

                case Type::PCM_AC3:
                    sys::read(*this,data,sizeof(Type::sample<Type::PCM_AC3>::buf));
                    return Type::sample<Type::PCM_AC3>::scale(util::get<Type::sample<Type::PCM_AC3>::buf>(data));

                case Type::PCM_FLOAT32:
                    sys::read(*this,data,sizeof(Type::sample<Type::PCM_FLOAT32>::buf));
                    return Type::sample<Type::PCM_FLOAT32>::scale(util::get<Type::sample<Type::PCM_FLOAT32>::buf>(data));

                default:
                    throw typename basic_streambuf<charT,traitT>::exception("get_scaled(): bad format of "+util::string_value(get_format()));
                }
                
            }

        protected:
            /**
             * The buffer, owned.
             *
             * Each derived stream installs its own subclass here and the base
             * destroys it.  It was a raw pointer new'd in every derived
             * constructor and deleted in ~basic_stream, which is sound only
             * because std::basic_iostream makes the whole hierarchy
             * uncopyable -- the ownership was correct but stated nowhere.
             */
            std::unique_ptr< basic_streambuf<charT,traitT> > m_buf;
        };
        
        typedef basic_stream<char> stream;
        
        
        template< typename charT, typename traitT >
        inline
        basic_streambuf<charT,traitT>::basic_streambuf() {
            m_get_buf.resize(BUF_SIZE);
            this->setg(m_get_buf.data(),m_get_buf.data(),m_get_buf.data());
            
            m_put_buf.resize(BUF_SIZE);
            this->setp(m_put_buf.data(),m_put_buf.data()+BUF_SIZE);

            m_eintr = false;
            m_length = pos_type(off_type(traits_type::eof()));
            m_pos = pos_type(off_type(0));

            
            set_samples_per_sec(44100);
            set_format(Type::PCM_S16_LE);
            set_channels(1);
        }
        
        template< typename charT, typename traitT >
        inline
        basic_streambuf<charT,traitT>::~basic_streambuf() {
            if(std::getenv("JLIB_MEDIA_STREAM_DEBUG"))
                std::cerr << "basic_streambuf<charT,traitT>::~basic_streambuf()"<<std::endl;
            close();
        }
        
        template< typename charT, typename traitT >
        inline
        typename basic_streambuf<charT,traitT>::int_type 
        basic_streambuf<charT,traitT>::overflow(int_type c) {
            if(std::getenv("JLIB_MEDIA_STREAM_DEBUG"))
                std::cerr << "jlib::media::basic_streambuf<charT,traitT>::overflow("<<c<<")"<<std::endl;
            if(this->pptr() >= this->epptr()) {
                if(this->sync() == traits_type::eof()) {
                    return traits_type::eof();
                }
            }
            
            *(this->pptr()) = c;
            this->pbump(1);
            return c;
        }

        template< typename charT, typename traitT >
        inline
        int basic_streambuf<charT,traitT>::get_bits_per_sample() const {
            return m_bits_per_sample;
        }

        template< typename charT, typename traitT >
        inline
        int basic_streambuf<charT,traitT>::get_channels() const {
            return m_channels;
        }

        template< typename charT, typename traitT >
        inline
        int basic_streambuf<charT,traitT>::get_samples_per_sec() const {
            return m_samples_per_sec;
        }

        template< typename charT, typename traitT >
        inline
        int basic_streambuf<charT,traitT>::get_format() const {
            return m_format;
        }

        template< typename charT, typename traitT >
        inline
        typename basic_streambuf<charT,traitT>::pos_type 
        basic_streambuf<charT,traitT>::get_length() const {
            return m_length;
        }

        template< typename charT, typename traitT >
        inline
        void basic_streambuf<charT,traitT>::set_bits_per_sample(int s) {
            m_bits_per_sample = s;
        }

        template< typename charT, typename traitT >
        inline
        void basic_streambuf<charT,traitT>::set_channels(int s) {
            m_channels = s;
        }

        template< typename charT, typename traitT >
        inline
        void basic_streambuf<charT,traitT>::set_samples_per_sec(int s) {
            m_samples_per_sec = s;
        }

        template< typename charT, typename traitT >
        inline
        void basic_streambuf<charT,traitT>::set_format(int s) {
            m_format = s;

            switch(get_format()) {
            case Type::PCM_U8:
            case Type::PCM_S8:
                m_bits_per_sample = 8;
                break;
            case Type::PCM_U16_LE:
            case Type::PCM_U16_BE:
            case Type::PCM_S16_LE:
            case Type::PCM_S16_BE:
                m_bits_per_sample = 16;
                break;
            case Type::PCM_FLOAT32:
                m_bits_per_sample = 32;
                break;
            default:
                throw exception("unknown format in set_format()");
            }
        }

        template< typename charT, typename traitT >
        inline
        void basic_streambuf<charT,traitT>::set_length(pos_type s) {
            m_length = s;
        }


        
        template< typename charT, typename traitT >
        inline
        void basic_streambuf<charT,traitT>::close() {
            if(std::getenv("JLIB_MEDIA_STREAM_DEBUG"))
                std::cerr << "jlib::media::basic_streambuf<charT,traitT>::close()"<<std::endl;
            
        }
        
        
        template< typename charT, typename traitT >
        inline
        void basic_stream<charT,traitT>::rewind() {
            if(std::getenv("JLIB_MEDIA_STREAM_DEBUG"))
                std::cerr << "jlib::media::basic_stream<charT,traitT>::rewind()"<<std::endl;
            this->clear();
            this->seekg(0);
        }

        template< typename charT, typename traitT >
        inline
        bool basic_streambuf<charT,traitT>::is_interrupted() const { 
            return m_eintr; 
        }
        
        template< typename charT, typename traitT >
        inline
        basic_stream<charT,traitT>::basic_stream() 
            : std::basic_iostream<charT,traitT>(NULL)
        {

        }
        
        template< typename charT, typename traitT >
        inline
        basic_stream<charT,traitT>::~basic_stream() {

        }

        template< typename charT, typename traitT >
        inline
        void basic_stream<charT,traitT>::close() {
            m_buf->close();
        }
        
        template< typename charT, typename traitT >
        inline
        int basic_stream<charT,traitT>::get_bits_per_sample() const {
            return m_buf->get_bits_per_sample();
        }

        template< typename charT, typename traitT >
        inline
        int basic_stream<charT,traitT>::get_channels() const {
            return m_buf->get_channels();
        }

        template< typename charT, typename traitT >
        inline
        int basic_stream<charT,traitT>::get_samples_per_sec() const {
            return m_buf->get_samples_per_sec();
        }

        template< typename charT, typename traitT >
        inline
        int basic_stream<charT,traitT>::get_format() const {
            return m_buf->get_format();
        }

        template< typename charT, typename traitT >
        inline
        typename basic_streambuf<charT,traitT>::pos_type 
        basic_stream<charT,traitT>::get_length() const {
            return m_buf->get_length();
        }

        template< typename charT, typename traitT >
        inline
        void basic_stream<charT,traitT>::set_bits_per_sample(int s) {
            m_buf->set_bits_per_sample(s);
        }

        template< typename charT, typename traitT >
        inline
        void basic_stream<charT,traitT>::set_channels(int s) {
            m_buf->set_channels(s);
        }

        template< typename charT, typename traitT >
        inline
        void basic_stream<charT,traitT>::set_samples_per_sec(int s) {
            m_buf->set_samples_per_sec(s);
        }

        template< typename charT, typename traitT >
        inline
        void basic_stream<charT,traitT>::set_format(int s) {
            m_buf->set_format(s);
        }

        template< typename charT, typename traitT >
        inline
        void basic_stream<charT,traitT>::set_length(pos_type s) {
            m_buf->set_length(s);
        }

        template< typename charT, typename traitT >
        inline
        bool basic_stream<charT,traitT>::is_interrupted() const {
            return m_buf->is_interrupted();
        }

    }
}


#endif // JLIB_MEDIA_STREAM_HH
