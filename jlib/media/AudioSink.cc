/* -*- mode: C++ c-basic-offset: 4 -*-
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
