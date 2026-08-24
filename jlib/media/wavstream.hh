/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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

#include <exception>
#include <iostream>
#include <string>
#include <cstring>

#include <jlib/media/WavFile.hh>
#include <jlib/media/stream.hh>

namespace jlib {
    namespace media {

        /**
         * A read-only stream over a WAV file's samples.
         *
         * The point of it is to cut out the middle man.  Getting a WAV's audio
         * into anything that takes a media::stream meant
         *
         *     WavFile in(path);
         *     datastream data(in.get_pcm());
         *     data.set<WavFile>(in);
         *
         * -- three objects, and the format had to be copied across by hand
         * afterwards or the samples would be interpreted wrongly.  A wavstream
         * is one object that holds the file, serves its samples, and takes its
         * format from the header, so there is nothing to remember and nothing
         * to get out of step.
         *
         * Read-only.  The class used to carry its own copies of WavFile's
         * three chunk builders, which were never called by anything, and
         * writing a WAV is what WavFile::save is for.  sync() reports failure
         * rather than quietly discarding anything written.
         */
        template< typename charT, typename traitT = std::char_traits<charT> >
        class basic_wavbuf : public basic_streambuf<charT,traitT> {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = std::string("jlib::media::basic_wavbuf::exception")+(msg==""?"":": ")+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            typedef charT                               char_type;
            typedef traitT                              traits_type;
            typedef typename traits_type::int_type      int_type;
            typedef typename traits_type::pos_type      pos_type;
            typedef typename traits_type::off_type      off_type;

            static const unsigned int BUF_SIZE = 1024;

            basic_wavbuf();
            explicit basic_wavbuf(const std::string& file);
            explicit basic_wavbuf(const WavFile& wav);

            virtual int_type underflow();
            virtual int_type sync();

            /**
             * Seeking, which this had none of.
             *
             * stream::rewind() is clear() followed by seekg(0), and without
             * these it went to the default streambuf implementations, which
             * refuse -- so rewind() set failbit on every wavstream and every
             * read after it returned nothing.  A datastream has always had
             * them, so the two halves of the same interface behaved
             * differently, and only the one nothing called was broken.
             *
             * PlayList::render calls rewind() on each roll before each hit, so
             * a wav-backed pattern would have rendered silence.  Nothing calls
             * PlayList::render, which is why this survived.
             */
            virtual pos_type seekoff(off_type, std::ios_base::seekdir,
                                     std::ios_base::openmode = std::ios_base::in);
            virtual pos_type seekpos(pos_type,
                                     std::ios_base::openmode = std::ios_base::in);

            /** The file this was loaded from, empty if it was handed a WavFile. */
            std::string get_file() const;

            /** Load a WAV and rewind to the start of its samples. */
            void set_file(const std::string& s);

            const WavFile& get_wav() const;
            void set_wav(const WavFile& w);

        protected:
            /**
             * Take the stream's format from the file's, and rewind.
             *
             * This is the part that made it worth finishing.  A datastream
             * knows nothing about what it holds, so its format has to be set
             * separately and can disagree with the samples; a wavstream reads
             * both from the same header.
             */
            void adopt();

            WavFile m_wav;
            std::string m_file;
            std::string::size_type m_p;
        };

        template<typename charT, typename traitT=std::char_traits<charT> >
        class basic_wavstream : public basic_stream<charT,traitT> {
        public:
            basic_wavstream();
            explicit basic_wavstream(const std::string& file);
            explicit basic_wavstream(const WavFile& wav);

            std::string get_file() const;
            void set_file(const std::string& s);

            const WavFile& get_wav() const;
            void set_wav(const WavFile& w);

        protected:
            basic_wavbuf<charT,traitT>* buf() const;
        };

        typedef basic_wavstream<char> wavstream;


        template< typename charT, typename traitT >
        inline
        basic_wavbuf<charT,traitT>::basic_wavbuf()
            : basic_streambuf<charT,traitT>(),
              m_p(0)
        {
            adopt();
        }

        template< typename charT, typename traitT >
        inline
        basic_wavbuf<charT,traitT>::basic_wavbuf(const std::string& file)
            : basic_streambuf<charT,traitT>(),
              m_p(0)
        {
            set_file(file);
        }

        template< typename charT, typename traitT >
        inline
        basic_wavbuf<charT,traitT>::basic_wavbuf(const WavFile& wav)
            : basic_streambuf<charT,traitT>(),
              m_wav(wav),
              m_p(0)
        {
            adopt();
        }

        template< typename charT, typename traitT >
        inline
        void basic_wavbuf<charT,traitT>::adopt() {
            this->set_format(m_wav.get_format());
            this->set_channels(m_wav.get_channels());
            this->set_samples_per_sec(m_wav.get_samples_per_sec());
            this->set_bits_per_sample(m_wav.get_bits_per_sample());
            this->set_length(m_wav.get_pcm().length());

            m_p = 0;
        }

        template< typename charT, typename traitT >
        inline
        std::string basic_wavbuf<charT,traitT>::get_file() const {
            return m_file;
        }

        template< typename charT, typename traitT >
        inline
        void basic_wavbuf<charT,traitT>::set_file(const std::string& file) {
            m_file = file;

            if(!m_file.empty())
                m_wav.load(m_file);

            adopt();
        }

        template< typename charT, typename traitT >
        inline
        const WavFile& basic_wavbuf<charT,traitT>::get_wav() const {
            return m_wav;
        }

        template< typename charT, typename traitT >
        inline
        void basic_wavbuf<charT,traitT>::set_wav(const WavFile& wav) {
            m_wav = wav;
            m_file.clear();

            adopt();
        }

        template< typename charT, typename traitT >
        inline
        typename basic_wavbuf<charT,traitT>::int_type
        basic_wavbuf<charT,traitT>::underflow() {
            // By reference.  get_pcm() returned by value when this was written,
            // which was free under the copy-on-write std::string of the time
            // and is a copy of the whole file now.
            const std::string& pcm = m_wav.get_pcm();

            if(m_p >= pcm.length())
                return traits_type::eof();

            const std::string::size_type left = pcm.length() - m_p;
            const std::string::size_type count =
                (left > BUF_SIZE) ? BUF_SIZE : left;

            std::memcpy(this->eback(), pcm.data() + m_p, count);
            this->setg(this->eback(), this->eback(), this->eback() + count);

            m_p += count;

            return traits_type::to_int_type(*this->gptr());
        }

        template< typename charT, typename traitT >
        inline
        typename basic_wavbuf<charT,traitT>::pos_type
        basic_wavbuf<charT,traitT>::seekoff(off_type o, std::ios_base::seekdir s,
                                            std::ios_base::openmode m) {
            // m_p counts what has been handed to the get area, so the position
            // actually reached is that less whatever is still sitting in it.
            const off_type held = (this->egptr() > this->gptr())
                ? (this->egptr() - this->gptr()) : 0;

            off_type pos;

            switch(s) {
            case std::ios_base::beg:
                pos = o;
                break;
            case std::ios_base::cur:
                pos = static_cast<off_type>(m_p) - held + o;
                break;
            case std::ios_base::end:
                pos = static_cast<off_type>(m_wav.get_pcm().length()) + o;
                break;
            default:
                return pos_type(off_type(-1));
            }

            return this->seekpos(pos_type(pos), m);
        }

        template< typename charT, typename traitT >
        inline
        typename basic_wavbuf<charT,traitT>::pos_type
        basic_wavbuf<charT,traitT>::seekpos(pos_type p,
                                            std::ios_base::openmode) {
            const off_type at = static_cast<off_type>(p);

            if(at < 0 ||
               at > static_cast<off_type>(m_wav.get_pcm().length()))
                return pos_type(off_type(-1));

            m_p = static_cast<std::string::size_type>(at);

            // Drop whatever was buffered, so the next read comes from the new
            // position rather than from what was already in hand.
            this->setg(this->eback(), this->eback(), this->eback());

            return p;
        }

        template< typename charT, typename traitT >
        inline
        typename basic_wavbuf<charT,traitT>::int_type
        basic_wavbuf<charT,traitT>::sync() {
            // Read-only.  Anything written would have nowhere to go, so say so
            // rather than swallow it; an empty put area syncs cleanly, which is
            // what flushing on destruction does.
            return (this->pptr() != this->pbase()) ? -1 : 0;
        }


        template< typename charT, typename traitT >
        inline
        basic_wavstream<charT,traitT>::basic_wavstream()
            : basic_stream<charT,traitT>()
        {
            this->m_buf.reset(new basic_wavbuf<charT,traitT>());
            this->init(this->m_buf.get());
        }

        template< typename charT, typename traitT >
        inline
        basic_wavstream<charT,traitT>::basic_wavstream(const std::string& file)
            : basic_stream<charT,traitT>()
        {
            this->m_buf.reset(new basic_wavbuf<charT,traitT>(file));
            this->init(this->m_buf.get());
        }

        template< typename charT, typename traitT >
        inline
        basic_wavstream<charT,traitT>::basic_wavstream(const WavFile& wav)
            : basic_stream<charT,traitT>()
        {
            this->m_buf.reset(new basic_wavbuf<charT,traitT>(wav));
            this->init(this->m_buf.get());
        }

        template< typename charT, typename traitT >
        inline
        basic_wavbuf<charT,traitT>* basic_wavstream<charT,traitT>::buf() const {
            if(!this->m_buf)
                throw typename basic_wavbuf<charT,traitT>::exception("m_buf == null");

            basic_wavbuf<charT,traitT>* b =
                dynamic_cast< basic_wavbuf<charT,traitT>* >(this->m_buf.get());

            if(!b)
                throw typename basic_wavbuf<charT,traitT>::exception("buf == null");

            return b;
        }

        template< typename charT, typename traitT >
        inline
        std::string basic_wavstream<charT,traitT>::get_file() const {
            return buf()->get_file();
        }

        template< typename charT, typename traitT >
        inline
        void basic_wavstream<charT,traitT>::set_file(const std::string& file) {
            buf()->set_file(file);
        }

        template< typename charT, typename traitT >
        inline
        const WavFile& basic_wavstream<charT,traitT>::get_wav() const {
            return buf()->get_wav();
        }

        template< typename charT, typename traitT >
        inline
        void basic_wavstream<charT,traitT>::set_wav(const WavFile& wav) {
            buf()->set_wav(wav);
        }

    }
}

#endif // JLIB_MEDIA_WAVSTREAM_HH
