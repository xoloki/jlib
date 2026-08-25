/* -*- mode: C++ c-basic-offset: 4 -*-
 * AudioFile.h
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

#ifndef JLIB_MEDIA_DSP_HH
#define JLIB_MEDIA_DSP_HH

#include <exception>
#include <string>
#include <cstring>
#include <sstream>

#include <jlib/media/stream.hh>

#include <errno.h>

namespace jlib {
    namespace media {
        
        class Dsp {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = "jlib::media::Dsp exception"+
                        (msg != "" ? (": "+msg):"");
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
                
                static void throw_errno(const std::string& msg) {
                    std::ostringstream o;
                    o << ((msg!="")?(msg+": "):"") << strerror(errno);
                    throw exception(o.str());
                }

            protected:
                std::string m_msg;
            };

            Dsp(const std::string& node="/dev/dsp",bool open=true);
            virtual ~Dsp();

            void open(const std::string& node="");

            void config(stream& s);
            void config(int bits_per_sample, int samples_per_sec, int channels, int format);

            void play(stream& s);
            void play_frag(stream& s,int n=1);

            void reset();

            bool is_configured() const;

            int get_fragsize() const;
            void set_fragsize(int f);

            int get_fragments() const;
            int get_frags_total() const;
            int get_frags_used() const;

            void close();
        protected:
            void write(const std::string& data);

            int m_bits_per_sample, m_samples_per_sec, m_channels, m_format;
            int m_fragsize;
            bool m_configured;
            int m_dsp;
            std::string m_node, m_buf;
        };

        
    }
}

#endif //JLIB_MEDIA_DSP_HH
