/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2020 Joey Yandle <xoloki@gmail.com>
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
#include <memory>
#include <sstream>

#include <cmath>

#include <jlib/media/mixer.hh>
#include <jlib/media/notestream.hh>
#include <jlib/media/PortAudioSink.hh>
#include <jlib/media/source_stream.hh>
#include <jlib/media/voice.hh>

#include <jlib/sys/sys.hh>
#include <jlib/util/util.hh>

void play(std::vector<std::string> song, int format, int channels);

int main(int argc, char** argv) {

    try {
        using namespace jlib::media;

        int format = Type::PCM_FLOAT32;
        int channels = 2;
        std::vector<std::string> melody;

        if(argc < 2) {
            std::cerr << "usage: " << argv[0] << " NOTE [NOTE ...]\n"
                      << "  a note is tone(letter)octave(number):beats, "
                      << "optionally /waveform\n"
                      << "  join notes with + to sound them together:\n"
                      << "    " << argv[0] << " C@3:1 E@3:1 G@3:1        "
                      << "three notes, one after another\n"
                      << "    " << argv[0] << " C@3:1+E@3:1+G@3:1        "
                      << "the same three as a chord\n";
            exit(1);
        }

        for(int i = 1; i < argc; i++)
            melody.push_back(argv[i]);

        play(melody, format, channels);
        
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }

    exit(0);
}

/**
 * Sound one entry of the melody, which may be several notes at once.
 *
 * Notes joined by + become voices in a mixer, and a mixer is a source like any
 * other, so a chord reaches the device by exactly the route a single note does.
 * That is the whole argument for the interface: nothing below here can tell how
 * many notes are sounding.
 *
 * Automatic staging, because this is generated rather than mixed -- there is no
 * engineer riding faders over a chord, and without it a triad would be louder
 * than the notes around it purely for having three of something.
 */
static void sound(jlib::media::PortAudioSink& dsp, const std::string& entry,
                  int format, int channels)
{
    using namespace jlib::media;

    const std::vector<std::string> notes = jlib::util::tokenize(entry, "+");

    const double rate = 44100;

    mixer m;
    m.set_staging(mixer::staging::automatic);
    m.set_rate(rate);     // the limiter turns its release time into samples

    // The voices have to outlive the render, and the mixer holds them, so this
    // is only here to spell out that they are not stack temporaries.
    for(const std::string& n : notes) {
        notestream ns(n);

        m.add(std::make_shared<voice>(ns.get_instrument(), ns.get_freq(),
                                      static_cast<unsigned long>(rate * ns.get_time()),
                                      rate));
    }

    source_stream live(&m);
    live.set_samples_per_sec(static_cast<unsigned int>(rate));
    live.set_format(format);
    live.set_channels(channels);

    dsp.play(live);
}

void play(std::vector<std::string> song, int format, int channels) {
    jlib::media::PortAudioSink dsp;

    for(const std::string& s : song)
        sound(dsp, s, format, channels);
}
