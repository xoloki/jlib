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

#ifndef JLIB_MEDIA_SOURCE_STREAM_HH
#define JLIB_MEDIA_SOURCE_STREAM_HH

#include <jlib/media/source.hh>
#include <jlib/media/stream.hh>

#include <cstdint>

#include <cmath>
#include <cstring>
#include <vector>

namespace jlib {
namespace media {

/**
 * A media::stream over a source.
 *
 * The one place float becomes PCM, and the reason the rest of the library needs
 * to know nothing about any of this: Player, PortAudioSink, WavFile and the
 * feeder thread all take streams, so wrapping a source in one makes a live mix
 * playable by every one of them unchanged.
 *
 * Unlike datastream and notestream this does not hold the whole thing up front.
 * It pulls a block from the source whenever the get area runs out, which is
 * what makes it usable for something being generated as it plays.  The
 * consequence is that it has no length and cannot seek; rewind() resets the
 * source, which is the only meaning seeking to zero can have here.
 */
template< typename charT, typename traitT = std::char_traits<charT> >
class basic_source_streambuf : public basic_streambuf<charT,traitT> {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = std::string("jlib::media::basic_source_streambuf::exception")+(msg==""?"":": ")+msg;
        }
        virtual ~exception() {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }
    protected:
        std::string m_msg;
    };

    typedef charT                               char_type;
    typedef traitT                              traits_type;
    typedef typename traits_type::int_type      int_type;

    /**
     * @param s       not owned; it has to outlive this
     * @param frames  how much to pull at a time
     */
    basic_source_streambuf(source* s = 0, unsigned long frames = 1024)
        : m_source(s),
          m_frames(frames)
    {
    }

    void set_source(source* s) { m_source = s; }
    source* get_source() const { return m_source; }

    virtual int_type underflow()
    {
        if(m_source == 0 || m_source->done())
            return traits_type::eof();

        const unsigned int channels = this->get_channels();
        const int frame = channels * (this->get_bits_per_sample() / 8);

        if(frame <= 0)
            throw exception("underflow(): the format says a frame is no bytes");

        m_mix.assign(m_frames * channels, 0);

        const unsigned long made = m_source->render(m_mix.data(), m_frames, channels);
        if(made == 0)
            return traits_type::eof();

        m_pcm.assign(made * frame, '\0');

        for(unsigned long i = 0; i < made * channels; i++)
            write_one(m_pcm, i * (frame / channels), m_mix[i]);

        this->setg(&m_pcm[0], &m_pcm[0], &m_pcm[0] + m_pcm.size());

        return traits_type::to_int_type(*this->gptr());
    }

    virtual void rewind()
    {
        if(m_source)
            m_source->reset();

        this->setg(0, 0, 0);
    }

protected:
    /**
     * One value, converted and clamped.
     *
     * The clamp is the point of doing this in one place.  A source is nominally
     * in [-1,1] but nothing enforces it -- a mix of several is routinely over,
     * and a truncated Fourier series overshoots on its own -- and llround of an
     * out-of-range value into an integer wraps rather than saturates, which
     * sounds far worse than the clipping it replaces.
     */
    void write_one(std::string& out, std::size_t at, Type::scaled v) const
    {
        if(v >  1) v =  1;
        if(v < -1) v = -1;

        switch(this->get_format()) {
        case Type::PCM_FLOAT32:
            // Not util::byte_copy: it takes a non-const pointer and casts the
            // constness away, so it cannot take the address of a local.
            std::memcpy(&out[at], &v, sizeof(v));
            return;

        case Type::PCM_U8:
            out[at] = static_cast<char>(
                static_cast<unsigned char>(std::llround(v * 127.0) + 128));
            return;

        case Type::PCM_S8:
            out[at] = static_cast<char>(std::llround(v * 127.0));
            return;

        case Type::PCM_S16_LE: {
            const std::int16_t s = static_cast<std::int16_t>(std::llround(v * 32767.0));
            out[at]     = static_cast<char>(s & 0xff);
            out[at + 1] = static_cast<char>((s >> 8) & 0xff);
            return;
        }

        case Type::PCM_S16_BE: {
            const std::int16_t s = static_cast<std::int16_t>(std::llround(v * 32767.0));
            out[at]     = static_cast<char>((s >> 8) & 0xff);
            out[at + 1] = static_cast<char>(s & 0xff);
            return;
        }

        case Type::PCM_U16_LE: {
            const std::uint16_t s =
                static_cast<std::uint16_t>(std::llround(v * 32767.0) + 32768);
            out[at]     = static_cast<char>(s & 0xff);
            out[at + 1] = static_cast<char>((s >> 8) & 0xff);
            return;
        }

        case Type::PCM_U16_BE: {
            const std::uint16_t s =
                static_cast<std::uint16_t>(std::llround(v * 32767.0) + 32768);
            out[at]     = static_cast<char>((s >> 8) & 0xff);
            out[at + 1] = static_cast<char>(s & 0xff);
            return;
        }

        default:
            throw exception("unsupported format");
        }
    }

    source* m_source;
    unsigned long m_frames;

    std::vector<Type::scaled> m_mix;
    std::string m_pcm;
};

template<typename charT, typename traitT = std::char_traits<charT> >
class basic_source_stream : public basic_stream<charT,traitT> {
public:
    basic_source_stream(source* s = 0, unsigned long frames = 1024)
        : basic_stream<charT,traitT>()
    {
        this->m_buf.reset(new basic_source_streambuf<charT,traitT>(s, frames));
        this->init(this->m_buf.get());
    }
};

typedef basic_source_stream<char> source_stream;

}
}

#endif // JLIB_MEDIA_SOURCE_STREAM_HH
