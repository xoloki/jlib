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

// The oscillator and the envelope.
//
// The claim worth checking is that the waveforms are band-limited.  A sawtooth
// drawn as a ramp has energy at every harmonic without limit, and everything
// above half the sample rate folds back down as an inharmonic tone that has
// nothing to do with the note -- quiet at the bottom of the keyboard,
// unmistakable at the top.  Building the wave by adding harmonics up to Nyquist
// and stopping cannot do that, because nothing above Nyquist is ever generated.
//
// So the test measures the spectrum and asks how much energy sits somewhere
// other than on a harmonic of the fundamental.  It also generates the same
// waveform the naive way and measures that, which is what makes the number mean
// something: without a case that fails, "small" is not evidence.
//
// The fundamentals are chosen to land exactly on a DFT bin.  A first attempt
// used round frequencies, and the harmonics smeared across neighbouring bins --
// which the classifier counted as off-harmonic energy and reported as aliasing
// that was not there.
#include <jlib/media/instrument.hh>
#include <jlib/media/notestream.hh>
#include <jlib/media/voice.hh>

#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace jlib::media;

static int failures = 0;
static const double pi = 3.14159265358979323846;

static void ok(const char* what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static void below(const char* what, double got, double limit) {
    const bool good = (got < limit);
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what << ": " << std::scientific
              << std::setprecision(2) << got << " < " << limit << "\n";
}

static std::vector<float> render(const instrument& i, double freq,
                                 unsigned long n, double rate) {
    voice v(i, freq, n, rate);
    std::vector<float> x;
    x.reserve(n);
    while(!v.done()) x.push_back(v.next());
    return x;
}

/** Fraction of spectral energy not sitting on a harmonic of the fundamental. */
static double off_harmonic(const std::vector<float>& x, int off, int n, int bins) {
    double harm = 0, junk = 0;

    for(int k = 1; k < n/2; k++) {
        double re = 0, im = 0;
        for(int i = 0; i < n; i++) {
            re += x[off+i] * std::cos(2*pi*k*i/n);
            im -= x[off+i] * std::sin(2*pi*k*i/n);
        }
        const double e = re*re + im*im;

        if(k % bins == 0) harm += e; else junk += e;
    }

    return (harm + junk > 0) ? junk / (harm + junk) : 0;
}

static void band_limiting() {
    std::cout << "band limiting:\n";

    const double rate = 44100;
    const int N = 2048;
    const double bin = rate / N;

    // 431Hz, 2153Hz, 4306Hz -- all exactly on a bin
    for(int mult : {20, 100, 200}) {
        const double freq = mult * bin;

        for(auto w : {instrument::wave::saw, instrument::wave::square}) {
            instrument inst;
            inst.set_wave(w);

            const unsigned long n = (unsigned long)rate;
            const std::vector<float> x = render(inst, freq, n, rate);

            // the same waveform the obvious way, which does alias
            std::vector<float> naive;
            naive.reserve(n);
            for(unsigned long i = 0; i < n; i++) {
                const double ph = std::fmod(freq * i / rate, 1.0);
                naive.push_back((float)(w == instrument::wave::saw
                                        ? (2*ph - 1) : (ph < 0.5 ? 1.0 : -1.0)));
            }

            const int off = (int)n/2 - N/2;
            const double add = off_harmonic(x, off, N, mult);
            const double nai = off_harmonic(naive, off, N, mult);

            std::string tag = instrument::name_of(w) + " at " +
                std::to_string((int)freq) + "Hz";

            below((tag + ", additive").c_str(), add, 1e-9);

            // and the measurement can tell the difference
            ok((tag + ", naive is worse").c_str(), nai > add * 1000,
               "naive " + std::to_string(nai));
        }
    }
}

static void envelope() {
    std::cout << "\nenvelope:\n";

    const double rate = 44100;
    const unsigned long n = (unsigned long)rate;

    {
        // Silence at both ends, whatever was asked for.  A note that stops
        // mid-swing clicks; this is the invariant audio_test.hh's check_pcm
        // has been asserting all along, here at the source.
        instrument inst;
        inst.set_envelope(instrument::envelope{0.0, 0.0, 1.0, 0.0});   // asks for none

        const std::vector<float> x = render(inst, 440, n, rate);

        ok("zero-length ramps still start silent", x.front() == 0.0f);
        ok("zero-length ramps still end silent", x.back() == 0.0f);
    }

    {
        instrument inst;
        inst.set_envelope(instrument::envelope{0.1, 0.1, 0.5, 0.1});
        inst.set_gain(1.0);

        const std::vector<float> x = render(inst, 440, n, rate);

        // peak somewhere in the attack, sustain level in the middle
        double peak_in_attack = 0;
        for(unsigned long i = 0; i < (unsigned long)(0.1*rate); i++)
            peak_in_attack = std::max(peak_in_attack, (double)std::fabs(x[i]));

        double peak_in_sustain = 0;
        for(unsigned long i = (unsigned long)(0.4*rate); i < (unsigned long)(0.6*rate); i++)
            peak_in_sustain = std::max(peak_in_sustain, (double)std::fabs(x[i]));

        ok("attack reaches full level", peak_in_attack > 0.95,
           std::to_string(peak_in_attack));
        ok("sustain holds the stated level",
           std::fabs(peak_in_sustain - 0.5) < 0.02, std::to_string(peak_in_sustain));
    }

    {
        // A note too short to hold attack, decay and release: the segments give
        // way proportionally rather than run past the end.
        instrument inst;
        inst.set_envelope(instrument::envelope{0.5, 0.5, 0.8, 0.5});

        const unsigned long tiny = (unsigned long)(rate * 0.1);
        const std::vector<float> x = render(inst, 440, tiny, rate);

        ok("a note shorter than its envelope still starts silent", x.front() == 0.0f);
        ok("a note shorter than its envelope still ends silent", x.back() == 0.0f);

        double peak = 0;
        for(float s : x) peak = std::max(peak, (double)std::fabs(s));
        ok("and still makes a sound", peak > 0.1, std::to_string(peak));
    }
}

static void levels() {
    std::cout << "\nlevels:\n";

    const double rate = 44100;
    const unsigned long n = (unsigned long)rate;

    double first_rms = 0;

    for(auto w : {instrument::wave::sine, instrument::wave::saw,
                  instrument::wave::square, instrument::wave::triangle}) {
        instrument inst;
        inst.set_wave(w);

        const std::vector<float> x = render(inst, 220, n, rate);

        double peak = 0, sumsq = 0;
        for(float s : x) { peak = std::max(peak, (double)std::fabs(s)); sumsq += (double)s*s; }
        const double rms = std::sqrt(sumsq / n);

        // Peak matters because create_data quantizes straight into an integer;
        // out of range would wrap rather than clip.
        ok((instrument::name_of(w) + " stays inside full scale").c_str(),
           peak < 1.0, std::to_string(peak));

        // Changing waveform should not change how loud the note is.  They are
        // scaled to match in RMS rather than in peak for that reason -- a square
        // at a given peak is 3dB louder than a sine at the same peak.
        if(first_rms == 0) first_rms = rms;
        else
            ok((instrument::name_of(w) + " matches sine in loudness").c_str(),
               std::fabs(rms - first_rms) / first_rms < 0.05,
               std::to_string(rms) + " against " + std::to_string(first_rms));
    }
}

static void note_strings() {
    std::cout << "\nnote strings:\n";

    {
        notestream n("A#@4:2/saw");
        ok("waveform override parses",
           n.get_instrument().get_wave() == instrument::wave::saw);
        ok("and the rest of the note survives it",
           std::fabs(n.get_time() - 2.0) < 1e-9, std::to_string(n.get_time()));
    }

    {
        notestream n("A");
        ok("no override leaves the instrument alone",
           n.get_instrument().get_wave() == instrument::wave::sine);
    }

    {
        bool threw = false;
        try { notestream n("A/wobble"); } catch(std::exception&) { threw = true; }
        ok("an unknown waveform is refused", threw);
    }

    {
        // used to be silently ignored: "C##" was a C
        bool threw = false;
        try { notestream n("C##"); } catch(std::exception&) { threw = true; }
        ok("a second accidental is refused", threw);
    }

    {
        // this constructor threw for years; nothing called it
        bool threw = false;
        try { notestream n; } catch(std::exception&) { threw = true; }
        ok("the default note works", !threw);
    }

    {
        // get_freq() was declared and never defined, so this did not link
        notestream n(440.0);
        ok("get_freq reports the frequency",
           std::fabs(n.get_freq() - 440.0) < 1e-9, std::to_string(n.get_freq()));
    }
}

int main() {
    band_limiting();
    envelope();
    levels();
    note_strings();

    return failures ? 1 : 0;
}
