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
#include <sstream>

#include <cmath>

#include <jlib/media/notestream.hh>
#include <jlib/media/PortAudioSink.hh>
#include <jlib/media/source_stream.hh>
#include <jlib/media/voice.hh>

#include <jlib/sys/sys.hh>

const long double 	PI = 3.14159265358979323846264338;

void play(double freq, int format, int channels, double t);
void play(const std::string& note, int format, int channels, double t);
void play(jlib::media::notestream& ns, int format, int channels, double t);
void play_live(jlib::media::notestream& ns, int format, int channels, double t);

int main(int argc, char** argv) {

    try {
        using namespace jlib::media;

        double freq = 220;
        int format = Type::PCM_S16_LE;
        int channels = 2;
        std::string note;
        double t = 5;
        bool given = false;      // was a duration actually asked for?
        bool isnote = false;
        bool live = false;

        // --live generates as it plays, through source_stream, instead of
        // rendering the note into a buffer first.
        int arg = 1;
        if(argc > 1 && std::string(argv[1]) == "--live") {
            live = true;
            arg = 2;
        }

        if(argc > arg) {
            isnote = !std::isdigit(argv[arg][0]);

            std::istringstream i(argv[arg]);

            if(isnote)
                i >> note;
            else
                i >> freq;
        }
        
        if(argc > arg + 1) {
            std::istringstream i(argv[arg + 1]);
            i >> t;
            given = true;
        }

        // A note string carries its own duration -- the :BEATS in A@2:2 -- so
        // only override it when one was actually asked for on the command line.
        // This used to call set_time() unconditionally, so "jnote A@2:2" played
        // for the default five seconds and the :2 was discarded.  A bare
        // frequency says nothing about duration, so the default stands there.
        if(isnote) {
            notestream ns(note);
            const double d = given ? t : ns.get_time();

            if(live) play_live(ns, format, channels, d);
            else     play(ns, format, channels, d);
        }
        else {
            notestream ns(freq);

            if(live) play_live(ns, format, channels, t);
            else     play(ns, format, channels, t);
        }
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }

    exit(0);
}

// note denotes the frequency
void play(const std::string& note, int format, int channels, double t) {
    jlib::media::notestream ns(note);
    play(ns, format, channels, t);
}

void play(double freq, int format, int channels, double t) {
    jlib::media::notestream ns(freq);
    play(ns, format, channels, t);
}

void play(jlib::media::notestream& ns, int format, int channels, double t) {
    jlib::media::PortAudioSink dsp;

    ns.set_format(format);
    ns.set_channels(channels);
    //ns.set_nearest_time(t);
    ns.set_time(t);

    dsp.play(ns);
    
}

/**
 * The same note, generated as it plays rather than rendered first.
 *
 * The point of source_stream, demonstrated: a voice is not a buffer and has no
 * length, and the sink neither knows nor cares -- it reads a stream, and this
 * is one.  Everything a live mix will need is here already, with one voice
 * instead of many.
 */
void play_live(jlib::media::notestream& ns, int format, int channels, double t) {
    using namespace jlib::media;

    ns.set_time(t);

    instrument inst = ns.get_instrument();

    voice v(inst, ns.get_freq(),
            static_cast<unsigned long>(44100 * t), 44100.0);

    source_stream live(&v);
    live.set_samples_per_sec(44100);
    live.set_format(format);
    live.set_channels(channels);

    PortAudioSink dsp;
    dsp.play(live);
}
