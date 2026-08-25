/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
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

// Read a WAV back through wavstream.
//
// The point of the class is that it needs no help: it holds the file, serves
// its samples, and takes its format from the same header the samples came
// from.  So the test writes a known WAV and checks that streaming it back
// gives identical bytes and a format nobody had to set by hand.
#include <jlib/media/WavFile.hh>
#include <jlib/media/notestream.hh>
#include <jlib/media/wavstream.hh>

#include <jlib/media/PortAudioSink.hh>

#include "audio_test.hh"

#include <cstdio>
#include <iostream>
#include <string>

using namespace jlib::media;
using namespace jlib::tests;

static int failures = 0;

static void check(const char* what, long got, long want) {
    const bool ok = (got == want);
    if(!ok) ++failures;
    std::cout << (ok ? "  ok   " : "  FAIL ") << what
              << ": got " << got << ", expected " << want << "\n";
}

int main(int argc, char** argv) {
    const audio_mode mode = get_audio_mode(argc, argv);
    const std::string path = "wavstream_test.wav";

    try {
        // a known signal, written through the path that already has a test
        std::string pcm;
        {
            notestream note(440.0);
            note.set_format(Type::PCM_S16_LE);
            note.set_channels(1);

            char buf[4096];
            while(note.good()) {
                note.read(buf, sizeof(buf));
                pcm.append(buf, note.gcount());
            }
        }

        if(pcm.empty()) {
            std::cerr << "notestream produced no samples" << std::endl;
            return 1;
        }

        {
            WavFile out;
            out.set_format(Type::PCM_S16_LE);
            out.set_channels(1);
            out.set_samples_per_sec(44100);
            out.set_bits_per_sample(16);
            out.set_pcm(pcm);
            out.save(path);
        }

        // and back, with nothing set by hand
        wavstream ws(path);

        check("format",          ws.get_format(),          Type::PCM_S16_LE);
        check("channels",        ws.get_channels(),        1);
        check("samples per sec", ws.get_samples_per_sec(), 44100);
        check("bits per sample", ws.get_bits_per_sample(), 16);

        std::string back;
        char buf[4096];
        while(ws.good()) {
            ws.read(buf, sizeof(buf));
            back.append(buf, ws.gcount());
        }

        check("bytes streamed", back.size(), pcm.size());
        check("bytes identical", back == pcm ? 1 : 0, 1);

        // constructing from a WavFile already in hand must agree
        {
            WavFile in(path);
            wavstream direct(in);

            check("from WavFile: format", direct.get_format(), Type::PCM_S16_LE);

            std::string other;
            while(direct.good()) {
                direct.read(buf, sizeof(buf));
                other.append(buf, direct.gcount());
            }
            check("from WavFile: identical", other == pcm ? 1 : 0, 1);
        }

        // rewind, which had no implementation behind it.
        //
        // stream::rewind() is clear() plus seekg(0), and basic_wavbuf had
        // neither seekoff nor seekpos -- so it fell through to the streambuf
        // defaults, which refuse, and rewind() left failbit set on every
        // wavstream with every subsequent read returning nothing.  A
        // datastream has always had them, so half the interface worked.
        //
        // Nothing in the tree called rewind() on a wavstream, which is how it
        // survived: PlayList::render does, on every roll before every hit, and
        // nothing calls PlayList::render either.
        {
            wavstream w(path);

            std::string first;
            while(w.good()) {
                w.read(buf, sizeof(buf));
                first.append(buf, w.gcount());
            }
            check("read to the end", first == pcm ? 1 : 0, 1);

            w.rewind();
            check("rewind leaves it usable", w.good() ? 1 : 0, 1);

            std::string second;
            while(w.good()) {
                w.read(buf, sizeof(buf));
                second.append(buf, w.gcount());
            }
            check("and it reads the same again", second == pcm ? 1 : 0, 1);

            // partway, too -- rewind is only the seek everything happens to use
            w.rewind();
            w.seekg(64);
            check("seeking partway works", w.good() ? 1 : 0, 1);

            std::string rest;
            while(w.good()) {
                w.read(buf, sizeof(buf));
                rest.append(buf, w.gcount());
            }
            check("and lands where it was asked",
                  rest == pcm.substr(64) ? 1 : 0, 1);
        }

        // read-only: a write must fail rather than vanish
        {
            wavstream w(path);
            w << "not audio";
            w.flush();
            check("writing sets a failbit", w.good() ? 0 : 1, 1);
        }

        if(failures == 0 && should_play(mode, Type::PCM_S16_LE)) {
            wavstream play(path);
            PortAudioSink dsp;
            dsp.play(play);
        }

        std::remove(path.c_str());
    }
    catch(std::exception& e) {
        std::cerr << "wavstream test: " << e.what() << std::endl;
        std::remove(path.c_str());
        return 1;
    }

    return failures ? 1 : 0;
}
