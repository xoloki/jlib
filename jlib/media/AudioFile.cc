/* -*- mode: C++ c-basic-offset: 4 -*-
 * AudioFile.C - source file for class AudioFile
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

#include <jlib/media/AudioFile.hh>

#include <utility>
#include <jlib/media/Type.hh>

#include <jlib/util/util.hh>

#include <jlib/sys/sys.hh>

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
        AudioFile::AudioFile(const std::string& filename)
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

        const std::string& AudioFile::get_pcm() const {
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

        void AudioFile::set_pcm(const std::string& pcm) {
            m_pcm = std::move(pcm);
        }

        void AudioFile::add_pcm(const std::string& pcm) {
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

        void AudioFile::load(const std::string& filename) {
            m_filename = std::move(filename);
        }

        void AudioFile::save(const std::string& filename) {
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

            // Only linear PCM is decoded; the compressed formats are
            // recognized purely so they can be rejected.  The branches for the
            // linear formats were all empty -- the actual work is below -- and
            // OSS's MU_LAW, A_LAW and IMA_ADPCM have no Type::PCM_ equivalent,
            // so those comparisons could never have matched.
            if(m_format == Type::PCM_MPEG) {
                throw exception("can't handle non-pcm format PCM_MPEG");
            }
            else if(m_format == Type::PCM_AC3) {
                throw exception("can't handle non-pcm format PCM_AC3");
            }

            if(m_bits_per_sample == 8)
                i += jlib::util::get<u_char>(m_pcm,p*m_bits_per_sample);
            else if(m_bits_per_sample == 16)
                i += jlib::util::get<short>(m_pcm,p*m_bits_per_sample);
            
            return i;
        }


    }
}
