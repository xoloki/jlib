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

#include <iostream>
#include <fstream>

#include <unistd.h>

#include <cmath>

#include <jlib/media/datastream.hh>
#include <jlib/media/notestream.hh>
#include <jlib/media/PortAudioSink.hh>

#include "audio_test.hh"

#include <jlib/sys/sys.hh>

const long double 	PI = 3.14159265358979323846264338;



int main(int argc, char** argv) {
    using namespace jlib::media;
    using namespace jlib::tests;

    const audio_mode mode = get_audio_mode(argc, argv);

    if(mode != AUDIO_SILENT && !PortAudioSink::have_output_device()) {
        std::cerr << "no audio output device, skipping" << std::endl;
        return 77;
    }

    try {
        const int format = Type::PCM_U8;

        notestream note(220.0);
        note.set_format(format);
        note.set_channels(1);
        note.set_time(0.5);

        std::string pcm;
        jlib::sys::read(note, pcm);

        if(!check_pcm(pcm, format, 1, "datastream U8"))
            return 1;

        if(should_play(mode, format)) {
                notestream again(220.0);
            again.set_format(format);
            again.set_channels(1);
            again.set_time(0.5);

            PortAudioSink dsp;
            dsp.play(again);
        }
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }

    exit(0);
}
