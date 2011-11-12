/* -*- mode: C++ c-basic-offset: 4 -*-
 * AudioFile.C - source file for class AudioFile
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

#include <jlib/media/AudioFile.hh>

#include <jlib/util/util.hh>

#include <jlib/sys/sys.hh>

#include <sys/soundcard.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>

#include <fstream>

const std::string DSP = "/dev/dsp";

namespace jlib {
    namespace media {
        AudioFile::AudioFile(std::string filename)
            : m_bits_per_sample(16),
              m_channels(1),
              m_samples_per_sec(44100),
              m_format(16),
              m_pos(0),
              m_filename(filename)
        {
            
        }

        AudioFile::~AudioFile() { 
        }
        
        int AudioFile::get_bits_per_sample() const {
            return m_bits_per_sample;
        }

        int AudioFile::get_channels() const {
            return m_channels;
        }

        int AudioFile::get_samples_per_sec() const {
            return m_samples_per_sec;
        }

        int AudioFile::get_format() const {
            return m_format;
        }

        std::string AudioFile::get_pcm() const {
            return m_pcm;
        }

        void AudioFile::set_bits_per_sample(int s) {
            m_bits_per_sample = s;
        }

        void AudioFile::set_channels(int s) {
            m_channels = s;
        }

        void AudioFile::set_samples_per_sec(int s) {
            m_samples_per_sec = s;
        }

        void AudioFile::set_format(int s) {
            m_format = s;
        }

        void AudioFile::set_pcm(std::string pcm) {
            m_pcm = pcm;
        }

        void AudioFile::add_pcm(std::string pcm) {
            m_pcm.append(pcm);
        }

        void AudioFile::clear_pcm() {
            m_pcm.clear();
        }

        void AudioFile::load() {
            load(m_filename);
        }

        void AudioFile::save() {
            save(m_filename);
        }

        void AudioFile::load(std::string filename) {
            m_filename = filename;
        }

        void AudioFile::save(std::string filename) {
            m_filename = filename;
            std::ofstream ofs(filename.c_str());
            ofs << m_pcm;
            ofs.close();
        }

        int AudioFile::get_sample_count() const {
            return m_pcm.length() / (m_bits_per_sample / 8);
        }

        int AudioFile::get(int p) const {
            int i=0;

            if(m_format == AFMT_S8) {

            }
            else if(m_format == AFMT_U8) {

            }
            else if(m_format == AFMT_S16_LE) {

            }
            else if(m_format == AFMT_S16_BE) {

            }
            else if(m_format == AFMT_U16_LE) {

            }
            else if(m_format == AFMT_U16_BE) {

            }
            else if(m_format == AFMT_MU_LAW) {
                throw exception("can't handle non-pcm format AFMT_MU_LAW");
            }
            else if(m_format == AFMT_A_LAW) {
                throw exception("can't handle non-pcm format AFMT_A_LAW");
            }
            else if(m_format == AFMT_IMA_ADPCM) {
                throw exception("can't handle non-pcm format AFMT_IMA_ADPCM");
            }
            else if(m_format == AFMT_MPEG) {
                throw exception("can't handle non-pcm format AFMT_MPEG");
            }
            else if(m_format == AFMT_AC3) {
                throw exception("can't handle non-pcm format AFMT_AC3");
            }

            if(m_bits_per_sample == 8)
                i += jlib::util::get<u_char>(m_pcm,p*m_bits_per_sample);
            else if(m_bits_per_sample == 16)
                i += jlib::util::get<short>(m_pcm,p*m_bits_per_sample);
            
            return i;
        }


    }
}
