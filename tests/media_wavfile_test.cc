/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2011 Joey Yandle <xoloki@gmail.com>
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
 */

// Round-trip a WAV through WavFile: synthesize a note, write it, read it back,
// check the format chunk survived, then play it.
//
// This used to load /usr/share/sounds/card_shuffle.wav, which does not exist
// on macOS and is not guaranteed anywhere.  Generating the fixture also makes
// the test cover the format chunk's field widths: nSamplesPerSec is a 4-byte
// field with nAvgBytesPerSec immediately after it, and reading or writing it
// as u_long ran straight through the neighbour.
#include <iostream>
#include <fstream>
#include <string>

#include <cstdio>

#include <jlib/media/datastream.hh>
#include <jlib/media/notestream.hh>
#include <jlib/media/WavFile.hh>
#include <jlib/media/PortAudioSink.hh>

#include "audio_test.hh"
#include <jlib/media/Type.hh>

#include <jlib/sys/sys.hh>

int main(int argc, char** argv) {
    using namespace jlib::media;
    using namespace jlib::tests;

    const audio_mode mode = get_audio_mode(argc, argv);

    if(mode != AUDIO_SILENT && !PortAudioSink::have_output_device()) {
        std::cerr << "no audio output device, skipping" << std::endl;
        return 77;
    }

    const int RATE = 44100;
    const int CHANNELS = 2;
    const int BITS = 16;
    const int FORMAT = Type::PCM_S16_LE;

    const std::string path = "media_wavfile_test.wav";

    try {
        // synthesize half a second of A440
        notestream note(440.0);
        note.set_format(FORMAT);
        note.set_channels(CHANNELS);
        note.set_samples_per_sec(RATE);
        note.set_time(0.5);

        std::string pcm;
        jlib::sys::read(note, pcm);

        if(pcm.empty()) {
            std::cerr << "notestream produced no samples" << std::endl;
            return 1;
        }

        {
            WavFile out;
            out.set_format_tag(1);   // WAV_FMT_PCM
            out.set_bits_per_sample(BITS);
            out.set_channels(CHANNELS);
            out.set_samples_per_sec(RATE);
            out.set_format(FORMAT);
            out.set_pcm(pcm);
            out.save(path);
        }

        WavFile in(path);
        in.load_data_chunks();

        int failures = 0;
        if(in.get_samples_per_sec() != RATE) {
            std::cerr << "samples_per_sec round-tripped as " << in.get_samples_per_sec()
                      << ", expected " << RATE << std::endl;
            ++failures;
        }
        if(in.get_channels() != CHANNELS) {
            std::cerr << "channels round-tripped as " << in.get_channels()
                      << ", expected " << CHANNELS << std::endl;
            ++failures;
        }
        if(in.get_bits_per_sample() != BITS) {
            std::cerr << "bits_per_sample round-tripped as " << in.get_bits_per_sample()
                      << ", expected " << BITS << std::endl;
            ++failures;
        }
        if(in.get_pcm().size() != pcm.size()) {
            std::cerr << "pcm round-tripped as " << in.get_pcm().size()
                      << " bytes, expected " << pcm.size() << std::endl;
            ++failures;
        }

        if(failures == 0 && should_play(mode, FORMAT)) {
                datastream data(in.get_pcm());
            data.set<WavFile>(in);

            PortAudioSink dsp;
            dsp.play(data);
        }

        std::remove(path.c_str());
        return failures ? 1 : 0;
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        std::remove(path.c_str());
        return 1;
    }
}
