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

// Verify the PCM notestream generates, for every format it supports.
//
// Silent by default -- pass --play for the clean formats, or --play-all for
// every one of them.  See audio_test.hh.
#include <iostream>
#include <string>

#include <jlib/media/notestream.hh>
#include <jlib/media/PortAudioSink.hh>
#include <jlib/media/Type.hh>

#include <jlib/sys/sys.hh>

#include "audio_test.hh"

using namespace jlib::media;
using namespace jlib::tests;

static int check(double freq, int format, int channels,
                 const std::string& tag, audio_mode mode)
{
    notestream note(freq);
    note.set_format(format);
    note.set_channels(channels);
    note.set_time(0.25);

    std::string pcm;
    jlib::sys::read(note, pcm);

    const std::string label =
        tag + " " + (channels > 1 ? "stereo" : "mono");

    if(!check_pcm(pcm, format, channels, label))
        return 1;

    if(should_play(mode, format)) {
        notestream again(freq);
        again.set_format(format);
        again.set_channels(channels);
        again.set_time(0.25);

        PortAudioSink dsp;
        dsp.play(again);
    }

    std::cout << label << ": " << pcm.size() << " bytes ok" << std::endl;
    return 0;
}

int main(int argc, char** argv) {
    const audio_mode mode = get_audio_mode(argc, argv);

    if(mode != AUDIO_SILENT && !PortAudioSink::have_output_device()) {
        std::cerr << "no audio output device, skipping" << std::endl;
        return 77;
    }

    int failures = 0;

    try {
        failures += check(110, Type::PCM_U8,      1, "Type::PCM_U8", mode);
        failures += check(220, Type::PCM_U8,      2, "Type::PCM_U8", mode);
        failures += check(110, Type::PCM_S8,      1, "Type::PCM_S8", mode);
        failures += check(440, Type::PCM_S16_LE,  1, "Type::PCM_S16_LE", mode);
        failures += check(880, Type::PCM_S16_LE,  2, "Type::PCM_S16_LE", mode);
        failures += check(440, Type::PCM_S16_BE,  1, "Type::PCM_S16_BE", mode);
        failures += check(440, Type::PCM_U16_LE,  1, "Type::PCM_U16_LE", mode);
        failures += check(440, Type::PCM_U16_BE,  1, "Type::PCM_U16_BE", mode);
        failures += check(440, Type::PCM_FLOAT32, 1, "Type::PCM_FLOAT32", mode);
        failures += check(880, Type::PCM_FLOAT32, 2, "Type::PCM_FLOAT32", mode);
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    return failures ? 1 : 0;
}
