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
                exception(const std::string& msg = "") {
                    m_msg = std::string("jlib::media::AudioBuffer::exception")+((msg!="")?": ":"")+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };


        protected:
            

        };
        
        class AudioFile {
        public:
            
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = std::string("jlib::media::AudioFile::exception")+((msg!="")?": ":"")+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            AudioFile(const std::string& filename="");
            ~AudioFile();
            
            int get_bits_per_sample() const; 
            int get_channels() const; 
            int get_samples_per_sec() const; 
            int get_format() const; 
            /**
             * The samples, by reference.
             *
             * This returned by value, which was free when it was written:
             * libstdc++'s std::string was copy-on-write then, so returning one
             * was a refcount bump.  C++11 outlawed that and gcc dropped it in
             * 5.0, which turned every call into a full copy of the audio --
             * measured, 861K for a five second stereo sample, and four times
             * that to get it as far as a datastream.
             *
             * The setters keep taking by value, but move rather than assign,
             * so passing a temporary costs nothing and passing a variable
             * costs one copy instead of two.
             */
            const std::string& get_pcm() const;
        
            int get_sample_count() const;

            void set_bits_per_sample(int s); 
            void set_channels(int s); 
            void set_samples_per_sec(int s); 
            void set_format(int s); 
            void set_pcm(const std::string& pcm);
            void add_pcm(const std::string& pcm);
            void clear_pcm();

            virtual void load();
            virtual void save();
            virtual void load(const std::string& filename);
            virtual void save(const std::string& filename);

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
