/* -*- mode: C++ c-basic-offset: 4 -*-
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

#include <jlib/media/AudioSink.hh>
#include <jlib/media/Type.hh>

#include <jlib/sys/sys.hh>

#include <sstream>

namespace jlib {
namespace media {

// 1024 frames is ~23ms at 44100, which is a reasonable compromise between
// latency and how often we have to come back and refill.
const int DEFAULT_PERIOD = 1024;

AudioSink::AudioSink()
    : m_samples_per_sec(-1),
      m_channels(-1),
      m_format(-1),
      m_period(DEFAULT_PERIOD),
      m_configured(false)
{
}

AudioSink::~AudioSink() {
}

int AudioSink::format_size(int format) {
    switch(format) {
    case Type::PCM_U8:
    case Type::PCM_S8:
        return 1;
    case Type::PCM_S16_LE:
    case Type::PCM_S16_BE:
    case Type::PCM_U16_LE:
    case Type::PCM_U16_BE:
        return 2;
    case Type::PCM_FLOAT32:
        return 4;
    default:
        {
            std::ostringstream o;
            o << "no sample size for format 0x" << std::hex << format;
            throw exception(o.str());
        }
    }
}

void AudioSink::config(stream& s) {
    config(s.get_samples_per_sec(), s.get_channels(), s.get_format());
}

bool AudioSink::is_configured() const {
    return m_configured;
}

int AudioSink::get_samples_per_sec() const {
    return m_samples_per_sec;
}

int AudioSink::get_channels() const {
    return m_channels;
}

int AudioSink::get_format() const {
    return m_format;
}

int AudioSink::get_frame_size() const {
    if(!m_configured)
        throw exception("get_frame_size() before config()");

    return format_size(m_format) * m_channels;
}

int AudioSink::get_period() const {
    return m_period;
}

void AudioSink::set_period(int frames) {
    if(frames <= 0)
        throw exception("period must be positive");

    m_period = frames;
}

void AudioSink::play(stream& s) {
    config(s);

    while(s) {
        play_frag(s);
    }

    // Deliberately no drain() here.  jmelody calls play() once per note on
    // one sink, and draining between them would put a gap in the melody.
    // close(), and so the destructor, drains -- which is what keeps the tail
    // of the last note from being cut off.  Dsp had neither: it closed the
    // device outright and truncated.
}

void AudioSink::play_frag(stream& s, int n) {
    if(!m_configured)
        throw exception("play_frag() before config()");

    std::string buf;
    jlib::sys::getstring(s, buf, n * m_period * get_frame_size());

    if(!buf.empty())
        write(buf);
}

}
}
