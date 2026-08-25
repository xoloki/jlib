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

// The source interface, and the one invariant it has to keep.
//
// Rendering N frames in a single call must produce exactly the same samples as
// rendering them in any sequence of smaller calls -- identical, bit for bit,
// not nearly.
//
// That is what makes an offline render reproducible and what makes it match
// what was heard live, even though the live path used whatever block size the
// device asked for and the render used whatever was convenient.  It is nearly
// free to keep, since everything is driven from a running frame count, and very
// expensive to recover once something has been written that carries state
// across a block boundary.  So it is asserted rather than assumed, before there
// is anything complicated enough to break it.
#include <jlib/media/instrument.hh>
#include <jlib/media/source_stream.hh>
#include <jlib/media/voice.hh>

#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace jlib::media;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Render a voice in blocks of the given size, into one buffer. */
static std::vector<Type::scaled> in_blocks(const instrument& inst, double freq,
                                           unsigned long total, double rate,
                                           unsigned long block,
                                           unsigned int channels) {
    voice v(inst, freq, total, rate);

    std::vector<Type::scaled> out(total * channels, 0);
    unsigned long at = 0;

    while(at < total) {
        const unsigned long want = std::min(block, total - at);
        const unsigned long made = v.render(&out[at * channels], want, channels);

        if(made == 0) break;
        at += made;
    }

    return out;
}

static void block_size_independence() {
    std::cout << "block size independence:\n";

    const double rate = 44100;
    const unsigned long total = 4096;

    for(auto w : {instrument::wave::sine, instrument::wave::saw,
                  instrument::wave::square, instrument::wave::triangle}) {
        instrument inst;
        inst.set_wave(w);
        inst.set_envelope(instrument::envelope{0.01, 0.02, 0.6, 0.03});

        const std::vector<Type::scaled> whole =
            in_blocks(inst, 431.0, total, rate, total, 1);

        // Awkward sizes on purpose: powers of two would hide anything that
        // depends on a block boundary landing somewhere convenient.
        for(unsigned long block : {1UL, 7UL, 64UL, 333UL, 1024UL}) {
            const std::vector<Type::scaled> split =
                in_blocks(inst, 431.0, total, rate, block, 1);

            unsigned long differ = 0;
            for(unsigned long i = 0; i < total; i++)
                if(std::memcmp(&whole[i], &split[i], sizeof(Type::scaled)) != 0)
                    ++differ;

            ok(instrument::name_of(w) + " in blocks of " + std::to_string(block),
               differ == 0,
               differ ? (std::to_string(differ) + " samples differ") : "");
        }
    }
}

static void additive_and_channels() {
    std::cout << "\nrender:\n";

    const double rate = 44100;
    const unsigned long n = 512;

    instrument inst;

    {
        // render adds rather than overwrites, which is what makes mixing free
        voice a(inst, 431.0, n, rate), b(inst, 431.0, n, rate);

        std::vector<Type::scaled> one(n, 0), two(n, 0);
        a.render(one.data(), n, 1);
        b.render(two.data(), n, 1);
        b.reset();
        b.render(two.data(), n, 1);          // a second time, into the same buffer

        unsigned long wrong = 0;
        for(unsigned long i = 0; i < n; i++)
            if(std::fabs(two[i] - 2*one[i]) > 1e-6) ++wrong;

        ok("rendering twice into a buffer sums", wrong == 0);
    }

    {
        // a mono voice fills every channel of the frame
        voice v(inst, 431.0, n, rate);

        std::vector<Type::scaled> st(n * 2, 0);
        v.render(st.data(), n, 2);

        unsigned long wrong = 0;
        for(unsigned long i = 0; i < n; i++)
            if(st[i*2] != st[i*2 + 1]) ++wrong;

        ok("a mono voice fills both channels", wrong == 0);
    }

    {
        voice v(inst, 431.0, n, rate);
        std::vector<Type::scaled> out(n * 4, 0);

        const unsigned long made = v.render(out.data(), n * 4, 1);

        ok("asking for more than there is returns what there was",
           made == n, std::to_string(made));
        ok("and the voice says it is done", v.done());
    }
}

static void through_a_stream() {
    std::cout << "\nas a stream:\n";

    const double rate = 44100;
    const unsigned long n = 4096;

    instrument inst;
    voice v(inst, 431.0, n, rate);

    source_stream s(&v);
    s.set_format(Type::PCM_S16_LE);
    s.set_channels(1);
    s.set_samples_per_sec((int)rate);

    std::string pcm;
    char buf[997];                            // not a multiple of the block size
    while(s.good()) {
        s.read(buf, sizeof(buf));
        pcm.append(buf, s.gcount());
    }

    ok("a source reads out as a stream", pcm.size() == n * 2,
       std::to_string(pcm.size()) + " bytes for " + std::to_string(n) + " frames");

    // the envelope reaches silence at both ends, as everything else asserts
    ok("starts silent", pcm.size() >= 2 && pcm[0] == 0 && pcm[1] == 0);
    ok("ends silent", pcm.size() >= 2 &&
       pcm[pcm.size()-2] == 0 && pcm[pcm.size()-1] == 0);

    {
        // clamping: a source over full scale must saturate, not wrap.  llround
        // of an out-of-range value into an int16 wraps, which is a far worse
        // noise than the clipping it replaces.
        instrument loud;
        loud.set_gain(4.0);                   // deliberately way over

        voice hot(loud, 431.0, 256, rate);
        source_stream ls(&hot);
        ls.set_format(Type::PCM_S16_LE);
        ls.set_channels(1);

        std::string out;
        char b2[512];
        while(ls.good()) { ls.read(b2, sizeof(b2)); out.append(b2, ls.gcount()); }

        long over = 0;
        for(std::size_t i = 0; i + 1 < out.size(); i += 2) {
            const std::int16_t s16 = (std::int16_t)((unsigned char)out[i] |
                                                    ((unsigned char)out[i+1] << 8));
            // a wrap shows up as a sign flip where the waveform was heading up
            if(s16 == -32768) ++over;
        }

        ok("an over-driven source clips rather than wrapping", over == 0,
           over ? (std::to_string(over) + " wrapped samples") : "");
    }
}

int main() {
    block_size_independence();
    additive_and_channels();
    through_a_stream();

    return failures ? 1 : 0;
}
