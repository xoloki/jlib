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

/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Test-only helpers for the media tests.
 *
 * These tests are silent by default.  What they are actually testing is the
 * PCM the stream layer produces, and that can be checked without opening an
 * output device -- so `make check` no longer plays anything, which is both
 * faster and considerably kinder to whoever is sitting in front of the
 * machine.
 *
 *     ./media_notestream_test              verify only, no sound
 *     ./media_notestream_test --play       play the clean formats
 *     ./media_notestream_test --play-all   play every format, including the
 *                                          8-bit ones, which are audibly
 *                                          rough by nature
 *
 * Note there is deliberately no locking here.  Audio output is exclusive, but
 * `make check` never passes --play, so nothing it runs opens a device; the
 * only way to overlap playback is to background several tests by hand.
 */

#ifndef JLIB_TESTS_AUDIO_TEST_HH
#define JLIB_TESTS_AUDIO_TEST_HH

#include <jlib/media/Type.hh>

#include <iostream>
#include <string>

#include <cstring>

namespace jlib {
namespace tests {

enum audio_mode {
    AUDIO_SILENT,    // verify the samples, open no device
    AUDIO_PLAY,      // play the formats that sound clean
    AUDIO_PLAY_ALL   // play everything, warts included
};

inline audio_mode get_audio_mode(int argc, char** argv) {
    for(int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if(arg == "--play-all")
            return AUDIO_PLAY_ALL;
        if(arg == "--play")
            return AUDIO_PLAY;
    }
    return AUDIO_SILENT;
}

/**
 * Is this format one of the ones worth putting through a speaker by default?
 *
 * The 8- and 16-bit unsigned formats are correct but coarse -- 8 bits is
 * ~48dB of signal-to-noise and the quantization staircase is plainly
 * audible -- so they are reserved for --play-all rather than inflicted on
 * anyone running --play.
 */
inline bool is_clean_format(int format) {
    return format == jlib::media::Type::PCM_S16_LE ||
           format == jlib::media::Type::PCM_FLOAT32;
}

inline bool should_play(audio_mode mode, int format) {
    if(mode == AUDIO_SILENT)
        return false;
    if(mode == AUDIO_PLAY_ALL)
        return true;
    return is_clean_format(format);
}

/**
 * Check generated PCM without playing it: non-empty, a whole number of
 * frames, and silent at both ends.
 *
 * That last one is the interesting property. A note lasts a fixed time
 * whatever its frequency, so freq*time is rarely a whole number of cycles;
 * without the envelope the waveform stops mid-swing and drops straight to
 * silence, which clicks. Checking both endpoints is what keeps that fixed.
 */
inline bool check_pcm(const std::string& pcm, int format, int channels,
                      const std::string& tag)
{
    int width = 0;
    switch(format) {
    case jlib::media::Type::PCM_U8:
    case jlib::media::Type::PCM_S8:       width = 1; break;
    case jlib::media::Type::PCM_S16_LE:
    case jlib::media::Type::PCM_S16_BE:
    case jlib::media::Type::PCM_U16_LE:
    case jlib::media::Type::PCM_U16_BE:   width = 2; break;
    case jlib::media::Type::PCM_FLOAT32:  width = 4; break;
    default:
        std::cerr << tag << ": unknown format 0x" << std::hex << format << std::endl;
        return false;
    }

    if(pcm.empty()) {
        std::cerr << tag << ": produced no samples" << std::endl;
        return false;
    }

    const std::size_t frame = static_cast<std::size_t>(width) * channels;
    if(pcm.size() % frame != 0) {
        std::cerr << tag << ": " << pcm.size() << " bytes is not a whole number of "
                  << frame << "-byte frames" << std::endl;
        return false;
    }

    // Silence is 0 for the signed and float formats, and the midpoint for
    // the unsigned ones -- so compare the first sample against the last
    // rather than against a constant.
    const unsigned char* p = reinterpret_cast<const unsigned char*>(pcm.data());
    const std::size_t last = pcm.size() - frame;

    if(format == jlib::media::Type::PCM_FLOAT32) {
        // Compare as values, not bytes. The envelope drives the final sample
        // to zero while sin() is still negative, so it lands on -0.0, whose
        // sign bit differs from +0.0 even though the two are equal and
        // equally silent.
        float first_v = 0, last_v = 0;
        std::memcpy(&first_v, p, sizeof(first_v));
        std::memcpy(&last_v, p + last, sizeof(last_v));

        if(first_v != last_v) {
            std::cerr << tag << ": starts at " << first_v << " and ends at "
                      << last_v << " -- the note will click" << std::endl;
            return false;
        }

        return true;
    }

    for(std::size_t i = 0; i < static_cast<std::size_t>(width); i++) {
        if(p[i] != p[last + i]) {
            std::cerr << tag << ": starts and ends at different levels"
                      << " (byte " << i << ": " << (int)p[i]
                      << " vs " << (int)p[last + i] << ")"
                      << " -- the note will click" << std::endl;
            return false;
        }
    }

    return true;
}

}
}

#endif //JLIB_TESTS_AUDIO_TEST_HH
