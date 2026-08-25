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

// Changing things while they sound.
//
// Everything before this branch fixed a voice's pitch and a mixer's faders
// before rendering started.  A live controller does neither, and the two ways
// that goes wrong are both discontinuities: a retune that restarts the waveform
// somewhere else, and a fader that steps between blocks.  Both are clicks.
#include <jlib/media/instrument.hh>
#include <jlib/media/mixer.hh>
#include <jlib/media/voice.hh>

#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
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

static double biggest_step(const std::vector<Type::scaled>& v,
                           unsigned long from, unsigned long to) {
    double worst = 0;
    for(unsigned long i = from; i + 1 < to && i + 1 < v.size(); i++)
        worst = std::max(worst, (double)std::fabs(v[i+1] - v[i]));
    return worst;
}

static void retuning_does_not_click() {
    std::cout << "retuning a sounding voice:\n";

    const double rate = 44100;
    const unsigned long half = 4410;

    instrument inst;
    voice v(inst, 220, rate);          // held, so the envelope is flat by then

    std::vector<Type::scaled> out(half * 2, 0);
    v.render(out.data(), half, 1);

    v.set_freq(330);
    v.render(out.data() + half, half, 1);

    // Away from the seam a sine moves by at most 2*pi*f/rate per sample: 0.031
    // at 220Hz and 0.047 at 330Hz, times the amplitude.  A phase jump would put
    // a step of order the amplitude itself right at the seam.
    const double before = biggest_step(out, 100, half - 1);
    const double after  = biggest_step(out, half + 1, half * 2 - 100);
    const double seam   = std::fabs(out[half] - out[half - 1]);

    std::cout << "     step before " << std::fixed << std::setprecision(5) << before
              << "   after " << after << "   at the seam " << seam << "\n";

    ok("the seam is no worse than the waveform itself", seam <= after * 1.5,
       std::to_string(seam) + " against " + std::to_string(after));

    // and it really did retune
    ok("and the new pitch is faster", after > before * 1.3,
       std::to_string(after) + " against " + std::to_string(before));

    // Setting the same frequency is not a change and must do nothing at all.
    {
        voice a(inst, 220, rate), b(inst, 220, rate);
        std::vector<Type::scaled> x(half, 0), y(half, 0);

        a.render(x.data(), half, 1);

        b.render(y.data(), half/2, 1);
        b.set_freq(220);
        b.render(y.data() + half/2, half - half/2, 1);

        ok("retuning to the same pitch changes nothing", x == y);
    }
}

static void holding_and_letting_go() {
    std::cout << "\nholding a voice and letting it go:\n";

    const double rate = 44100;

    instrument inst;
    instrument::envelope e;
    e.attack = 0.01;
    e.decay = 0;
    e.sustain = 1.0;
    e.release = 0.05;
    inst.set_envelope(e);

    voice v(inst, 220, rate);

    ok("a held voice is not done before it starts", !v.done());

    std::vector<Type::scaled> out(static_cast<unsigned long>(rate), 0);
    const unsigned long made = v.render(out.data(), out.size(), 1);

    ok("it fills whatever it is given", made == out.size(),
       std::to_string(made));
    ok("and is still not done after a second", !v.done());

    auto peak = [&](unsigned long from, unsigned long n) {
        double p = 0;
        for(unsigned long i = from; i < from + n && i < out.size(); i++)
            p = std::max(p, (double)std::fabs(out[i]));
        return p;
    };

    ok("it starts from silence", std::fabs(out[0]) < 1e-6, std::to_string(out[0]));
    ok("and holds at its sustain level",
       std::fabs(peak(rate/2, 2000) - peak(rate/4, 2000)) < 0.01,
       std::to_string(peak(rate/4, 2000)) + " then " + std::to_string(peak(rate/2, 2000)));

    // now let it go
    v.release();
    ok("released, it is not done yet", !v.done());

    const unsigned long tail = static_cast<unsigned long>(e.release * rate) + 10;
    std::vector<Type::scaled> fade(tail, 0);
    v.render(fade.data(), tail, 1);

    ok("it fades to silence", std::fabs(fade[tail-1]) < 1e-6,
       std::to_string(fade[tail-1]));
    ok("and then it is done", v.done());

    // Releasing during the attack must fade from where the envelope had got
    // to, not jump up to the sustain level first.
    //
    // Compared against a voice released once it has reached sustain, rather
    // than against a number: the first version of this compared the peak of the
    // release tail against the last *sample* before it, which at 220Hz happened
    // to be near a zero crossing, and so demanded that a fade from 0.081 stay
    // under 0.003.  The envelope level and a sample of the waveform are not the
    // same quantity.
    {
        voice early(inst, 220, rate);
        std::vector<Type::scaled> a(100, 0);
        early.render(a.data(), 100, 1);      // 100 frames into a 441 frame attack
        early.release();

        std::vector<Type::scaled> b(tail, 0);
        early.render(b.data(), tail, 1);

        voice late(inst, 220, rate);
        std::vector<Type::scaled> c(static_cast<unsigned long>(rate/2), 0);
        late.render(c.data(), c.size(), 1);  // well past the attack
        late.release();

        std::vector<Type::scaled> d(tail, 0);
        late.render(d.data(), tail, 1);

        auto loudest = [](const std::vector<Type::scaled>& v) {
            double p = 0;
            for(Type::scaled s : v) p = std::max(p, (double)std::fabs(s));
            return p;
        };

        const double from_attack  = loudest(b);
        const double from_sustain = loudest(d);

        std::cout << "     released mid-attack, fades from " << std::fixed
                  << std::setprecision(4) << from_attack
                  << ";  released at sustain, from " << from_sustain << "\n";

        ok("released at sustain it fades from the sustain level",
           std::fabs(from_sustain - inst.get_gain()) < 0.02,
           std::to_string(from_sustain));

        ok("released mid-attack it fades from where it had got to",
           from_attack < from_sustain * 0.3,
           std::to_string(from_attack / from_sustain) + " of it");
    }
}

static void moving_a_fader_does_not_click() {
    std::cout << "\nmoving a fader while it plays:\n";

    const double rate = 44100;
    const unsigned long block = 1024;

    instrument inst;

    // the same voice twice: one through the mixer, one bare for reference
    auto through = std::make_shared<voice>(inst, 220, rate);
    voice bare(inst, 220, rate);

    mixer m;
    m.set_staging(mixer::staging::none);
    m.set_limiting(false);
    m.add(through, 1.0);

    std::vector<Type::scaled> mixed(block * 2, 0), ref(block * 2, 0);

    m.render(mixed.data(), block, 1);
    bare.render(ref.data(), block * 2, 1);

    m.set_gain(0, 0.5);
    m.render(mixed.data() + block, block, 1);

    // Across the second block the applied gain should walk from 1 to 0.5.
    double first = 0, last = 0;
    bool monotone = true, previous_set = false;
    double previous = 0;

    for(unsigned long i = 0; i < block; i++) {
        if(std::fabs(ref[block + i]) < 0.2)
            continue;

        const double g = mixed[block + i] / ref[block + i];

        if(!previous_set) { first = g; previous_set = true; }
        else if(g > previous + 1e-6) monotone = false;

        previous = g;
        last = g;
    }

    std::cout << "     gain across the block: " << std::fixed << std::setprecision(4)
              << first << " to " << last << "\n";

    ok("it starts near where the fader was", first > 0.95, std::to_string(first));
    ok("and arrives at where it now is", std::fabs(last - 0.5) < 0.01,
       std::to_string(last));
    ok("without going back up on the way", monotone);

    const double seam = std::fabs(mixed[block] - mixed[block - 1]);
    const double typical = biggest_step(mixed, 100, block - 1);

    ok("so the block boundary is not a step", seam <= typical * 1.5,
       std::to_string(seam) + " against " + std::to_string(typical));

    // And a fader nobody touches must cost nothing: an offline render has to
    // come out exactly as it did before ramping existed.
    {
        auto still = std::make_shared<voice>(inst, 220, rate);
        voice plain(inst, 220, rate);

        mixer q;
        q.set_staging(mixer::staging::none);
        q.set_limiting(false);
        q.add(still, 0.75);

        std::vector<Type::scaled> a(block * 2, 0), b(block * 2, 0);

        q.render(a.data(), block, 1);
        q.render(a.data() + block, block, 1);

        plain.render(b.data(), block * 2, 1);
        for(Type::scaled& s : b) s = static_cast<Type::scaled>(s * 0.75);

        ok("an untouched fader is exact", a == b);
    }
}

int main() {
    std::cout << std::unitbuf;

    retuning_does_not_click();
    holding_and_letting_go();
    moving_a_fader_does_not_click();

    return failures ? 1 : 0;
}
