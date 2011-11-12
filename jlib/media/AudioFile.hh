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

#ifndef JLIB_MEDIA_AUDIOFILE_HH
#define JLIB_MEDIA_AUDIOFILE_HH

#include <string>
#include <exception>

namespace jlib {
    namespace media {

        class AudioBuffer {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = std::string("jlib::media::AudioBuffer::exception")+((msg!="")?": ":"")+msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };


        protected:
            

        };
        
        class AudioFile {
        public:
            
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = std::string("jlib::media::AudioFile::exception")+((msg!="")?": ":"")+msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            AudioFile(std::string filename="");
            ~AudioFile();
            
            int get_bits_per_sample() const; 
            int get_channels() const; 
            int get_samples_per_sec() const; 
            int get_format() const; 
            std::string get_pcm() const;
        
            int get_sample_count() const;

            void set_bits_per_sample(int s); 
            void set_channels(int s); 
            void set_samples_per_sec(int s); 
            void set_format(int s); 
            void set_pcm(std::string pcm);
            void add_pcm(std::string pcm);
            void clear_pcm();

            virtual void load();
            virtual void save();
            virtual void load(std::string filename);
            virtual void save(std::string filename);

            /**
             * get the p'th sample from the pcm data
             */
            virtual int get(int p) const;

            template<class T>
            void set(const T& t) {
                set_bits_per_sample(t.get_bits_per_sample());
                set_channels(t.get_channels());
                set_samples_per_sec(t.get_samples_per_sec());
                set_format(t.get_format());
            }

        protected:
            int m_bits_per_sample, m_channels, m_samples_per_sec, m_format;
            int m_pos;
            std::string m_pcm;
            std::string m_filename;
        };
    }
}

#endif
