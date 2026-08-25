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

#ifndef JLIB_MEDIA_WAVFILE_HH
#define JLIB_MEDIA_WAVFILE_HH

#include <map>
#include <list>

#include <jlib/media/AudioFile.hh>

namespace jlib {
    namespace media {
        class WavFile : public AudioFile {
        public:
            WavFile();
            WavFile(const std::string& filename, bool load_data=true);
            virtual ~WavFile();

            int get_format_tag() const;
            
            void set_format_tag(int s);

            virtual void load(const std::string& filename);
            virtual void save(const std::string& filename);

            void load_data_chunks();

            std::string create_riff_header();
            std::string create_format_chunk();
            std::string create_data_chunk();

        protected:
            void parse_chunks();
            void parse_fmt();

            int m_format_tag;
            /**
             * map the chunkID to a list of pairs of stream offset and chunk size
             */
            std::map<std::string, std::list< std::pair<unsigned int,unsigned int> > > m_chunk_info;
        };
    }
}

#endif //JLIB_MEDIA_WAVFILE_HH
