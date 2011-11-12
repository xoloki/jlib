/* -*- mode: C++ c-basic-offset: 4 -*-
 * WavFile.C - source file for class WavFile
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

#include <jlib/media/WavFile.hh>
#include <jlib/media/Type.hh>

#include <jlib/util/util.hh>

#include <jlib/sys/sys.hh>

#include <cstdio>
#include <cstdlib>

#include <fstream>

#include <sys/soundcard.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

const std::string DSP = "/dev/dsp";

const int GROUP_ID_SIZE   = 0x8;
const int GROUP_ID_OFFSET = 0x0;

const int RIFF_TYPE_SIZE   = 0x4;
const int RIFF_TYPE_OFFSET = GROUP_ID_SIZE;

const int CHUNK_ID_SIZE   = 0x4;
const int CHUNK_ID_OFFSET = 0x0;

const int CHUNK_SIZE_SIZE   = 0x4;
const int CHUNK_SIZE_OFFSET = CHUNK_ID_SIZE;

const std::string CHUNK_ID_FORMAT = "fmt ";
const std::string CHUNK_ID_DATA = "data";

const int WAV_FMT_PCM = 0x1;
const int WAV_FMT_OKI_ADPCM = 0x10;

const int FORMAT_TAG_OFFSET            = 0x0;
const int CHANNELS_OFFSET              = 0x2;
const int SAMPLES_PER_SEC_OFFSET       = 0x4;
const int AVERAGE_BYTES_PER_SEC_OFFSET = 0x8;
const int BLOCK_ALIGN_OFFSET           = 0xc;
const int BITS_PER_SAMPLE_OFFSET       = 0xe;

const int FMT_CHUNK_SIZE = 0x10;

namespace jlib {
    namespace media {
        WavFile::WavFile() {
            set_format_tag(WAV_FMT_PCM);
        }

        WavFile::WavFile(std::string filename, bool load_data) {
            set_format_tag(WAV_FMT_PCM);
            m_filename = filename;
            parse_chunks();
            parse_fmt();
            if(load_data)
                load_data_chunks();
        }
        
        WavFile::~WavFile() { 
        }
        
        void WavFile::parse_chunks() {
            if(std::getenv("JLIB_MEDIA_WAVFILE_DEBUG")) {
                std::cerr << "enter jlib::media::WavFile::parse_chunks()" << std::endl;
            }
            std::string groupID, riffType, chunkID, chunkSize;
            if(std::getenv("JLIB_MEDIA_WAVFILE_DEBUG")) {
                std::cerr << "\topening " << m_filename << std::endl;
            }
            std::ifstream stream(m_filename.c_str());
            if(!stream) {
                throw AudioFile::exception("error opening file '"+m_filename+"'");
            }
            
            if(std::getenv("JLIB_MEDIA_WAVFILE_DEBUG")) {
                std::cerr << "\treading groupID " << std::endl;
            }
            sys::getstring(stream, groupID, GROUP_ID_SIZE);
            if(groupID.substr(0,4) != "RIFF") {
                throw AudioFile::exception("unknown groupID "+groupID);
            }

            if(std::getenv("JLIB_MEDIA_WAVFILE_DEBUG")) {
                std::cerr << "\treading riffType " << std::endl;
            }
            sys::getstring(stream, riffType, 4);
            if(riffType.substr(0,4) != "WAVE") {
                throw AudioFile::exception("unknown riffType "+riffType);
            }

            u_int pos, size;
            while(stream) {
                sys::getstring(stream, chunkID, CHUNK_ID_SIZE);
                sys::getstring(stream, chunkSize, CHUNK_SIZE_SIZE);

                if(stream) {
                    pos = stream.tellg();
                    size = util::get<u_int>(chunkSize);
                    
                    if(std::getenv("JLIB_MEDIA_WAVFILE_DEBUG")) {
                        std::cerr << std::endl;
                        std::cerr << "\tid   = " << chunkID << std::endl;
                        std::cerr << "\tpos  = " << pos << std::endl;
                        std::cerr << "\tsize = " << size << std::endl;
                    }
                    
                    m_chunk_info[chunkID].push_back(std::make_pair(pos,size));
                    stream.seekg(size, std::ios_base::cur);
                }
            }

            stream.close();
            if(std::getenv("JLIB_MEDIA_WAVFILE_DEBUG")) {
                std::cerr << "leave jlib::media::WavFile::parse_chunks()" << std::endl;
            }
        }

        void WavFile::parse_fmt() {
            std::ifstream stream(m_filename.c_str());
            std::list< std::pair<u_int,u_int> >& fmt_list = m_chunk_info[CHUNK_ID_FORMAT];
            if(fmt_list.size() < 1) {
                throw AudioFile::exception("error in jlib::media::WavFile::parse_fmt(): no fmt chunk");
            }
            else if(fmt_list.size() > 1) {
                std::cerr << "warning in jlib::media::WavFile::parse_fmt(): " 
                          << fmt_list.size() << " fmt chunks available, using first" << std::endl;
            }

            u_int pos  = fmt_list.front().first;
            u_int size = fmt_list.front().second;

            std::string chunk;
            stream.seekg(pos, std::ios_base::beg);
            sys::getstring(stream, chunk, size);
            
            m_format_tag = util::get<short>(chunk, FORMAT_TAG_OFFSET);
            m_bits_per_sample = util::get<u_short>(chunk, BITS_PER_SAMPLE_OFFSET);
            m_samples_per_sec = util::get<u_long>(chunk, SAMPLES_PER_SEC_OFFSET);
            m_channels = util::get<u_short>(chunk, CHANNELS_OFFSET);

            if(m_format_tag != WAV_FMT_PCM && m_format_tag != WAV_FMT_OKI_ADPCM) 
                throw AudioFile::exception("Unknown WAV format tag: "+util::valueOf(m_format_tag));

            if(m_format_tag == WAV_FMT_PCM) {
                
            }
            else if(m_format_tag == WAV_FMT_OKI_ADPCM) {
                m_bits_per_sample = 8;
            }
            
            if(m_bits_per_sample == 16) {
                m_format = Type::PCM_S16_LE;
            }
            else if(m_bits_per_sample == 8) {
                m_format = Type::PCM_U8;
            }
            else {
                throw 
                    AudioFile::exception("Unknown number of bits per sample: "+util::valueOf(m_bits_per_sample));
            }
            /*
              stream >> wChannels;
              stream >> dwSamplesPerSec;
              stream >> dwAvgBytesPerSec;
              stream >> wBlockAlign;
              stream >> wBitsPerSample;
              
              bufSize = dwSamplesPerSec*wBlockAlign/4;
            */
        }
        

        void WavFile::load_data_chunks() {
            if(std::getenv("JLIB_MEDIA_WAVFILE_DEBUG")) {
                std::cerr << "enter jlib::media::WavFile::get_data_chunks()" << std::endl;
            }
            std::ifstream stream(m_filename.c_str());
            std::list< std::pair<u_int,u_int> >& data_list = m_chunk_info[CHUNK_ID_DATA];
            if(data_list.size() < 1) {
                throw exception("error in jlib::media::WavFile::get_data_chunks(): no data chunk");
            }
            else if(data_list.size() > 1) {
                std::cerr << "warning in jlib::media::WavFile::get_data_chunks(): " 
                          << data_list.size() << " data chunks available, getting all" << std::endl;
            }

            clear_pcm();
            std::list< std::pair<u_int,u_int> >::iterator i = data_list.begin();
            std::string buffer;

            while(i != data_list.end()) {
                u_int pos  = i->first;
                u_int size = i->second;
                i++;
            
                stream.seekg(pos, std::ios_base::beg);
                sys::getstring(stream, buffer, size);
            
                add_pcm(buffer);
            }
        }

        int WavFile::get_format_tag() const {
            return m_format_tag;
        }

        void WavFile::set_format_tag(int s) {
            m_format_tag = s;
        }

        void WavFile::load(std::string filename) {
            m_filename = filename;
            parse_chunks();
            parse_fmt();

            load_data_chunks();
        }

        void WavFile::save(std::string filename) {
            m_filename = filename;
            std::ofstream ofs(filename.c_str());
            ofs << create_riff_header() << create_format_chunk() << create_data_chunk();
            ofs.close();
        }

        std::string WavFile::create_riff_header() {
            std::string chunk(GROUP_ID_SIZE+RIFF_TYPE_SIZE, '\0');

            chunk.replace(0,GROUP_ID_SIZE,"RIFF\0\0\0\0");
            chunk.replace(RIFF_TYPE_OFFSET,RIFF_TYPE_SIZE,"WAVE");
            
            return chunk;
        }

        std::string WavFile::create_format_chunk() {
            std::string chunk(CHUNK_ID_SIZE+CHUNK_SIZE_SIZE+FMT_CHUNK_SIZE, '\0');
            int base = CHUNK_ID_SIZE+CHUNK_SIZE_SIZE;

            chunk.replace(0,CHUNK_ID_SIZE,CHUNK_ID_FORMAT);
            util::set<int>(chunk,FMT_CHUNK_SIZE,CHUNK_SIZE_OFFSET);

            util::set<short>(chunk,get_format_tag(),base+FORMAT_TAG_OFFSET);
            util::set<u_short>(chunk,get_bits_per_sample(),base+BITS_PER_SAMPLE_OFFSET);
            util::set<u_long>(chunk,get_samples_per_sec(),base+SAMPLES_PER_SEC_OFFSET);
            util::set<u_short>(chunk,get_channels(),base+CHANNELS_OFFSET);
   
            return chunk;
        }

        std::string WavFile::create_data_chunk() {
            std::string chunk(CHUNK_ID_SIZE+CHUNK_SIZE_SIZE, '\0');
            
            chunk.replace(0,CHUNK_ID_SIZE,CHUNK_ID_DATA);
            util::set<u_int>(chunk,get_pcm().length(),CHUNK_SIZE_OFFSET);

            chunk.append(get_pcm());

            return chunk;
        }


    }
}
