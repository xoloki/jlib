/* -*- mode: C++ c-basic-offset: 4 -*-
 * AudioFile.h
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
                exception(std::string msg = "") {
                    m_msg = "jlib::media::Dsp exception"+
                        (msg != "" ? (": "+msg):"");
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
                
                static void throw_errno(std::string msg) {
                    std::ostringstream o;
                    o << ((msg!="")?(msg+": "):"") << strerror(errno);
                    throw exception(o.str());
                }

            protected:
                std::string m_msg;
            };

            Dsp(std::string node="/dev/dsp",bool open=true);
            virtual ~Dsp();

            void open(std::string node="");

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
            void write(std::string data);

            int m_bits_per_sample, m_samples_per_sec, m_channels, m_format;
            int m_fragsize;
            bool m_configured;
            int m_dsp;
            std::string m_node, m_buf;
        };

        
    }
}

#endif //JLIB_MEDIA_DSP_HH
