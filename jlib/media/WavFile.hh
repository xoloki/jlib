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
            WavFile(std::string filename, bool load_data=true);
            virtual ~WavFile();

            int get_format_tag() const;
            
            void set_format_tag(int s);

            virtual void load(std::string filename);
            virtual void save(std::string filename);

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
