// Tables, and what using one costs.
//
// A table is a cache of the same sum voice does directly, so the band limiting
// is identical and neither aliases.  What a table adds is interpolation error
// and banding error, and the point of this test is that both are numbers rather
// than assurances -- an offline render can then be exact and a live one can be
// cheap, with the difference between them stated.
#include <jlib/media/instrument.hh>
#include <jlib/media/voice.hh>
#include <jlib/media/wavetable.hh>

#include <chrono>
#include <cmath>
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

static std::vector<Type::scaled> render(const instrument& inst, double freq,
                                        unsigned long n, double rate,
                                        voice::quality q, wavetable_set* set) {
    voice v(inst, freq, n, rate);
    v.set_quality(q, set);

    std::vector<Type::scaled> out(n, 0);
    v.render(out.data(), n, 1);
    return out;
}

static const double PI = 3.14159265358979323846;

/** Magnitude spectrum of a block. */
static std::vector<double> spectrum(const std::vector<Type::scaled>& x,
                                    std::size_t off, int n) {
    std::vector<double> mag(n/2, 0.0);
    for(int k = 0; k < n/2; k++) {
        double re = 0, im = 0;
        for(int i = 0; i < n; i++) {
            re += x[off+i] * std::cos(2*PI*k*i/n);
            im -= x[off+i] * std::sin(2*PI*k*i/n);
        }
        mag[k] = std::sqrt(re*re + im*im);
    }
    return mag;
}

/**
 * How far below the signal the two renders differ, counting only what can be
 * heard.
 *
 * Comparing whole waveforms sample by sample answers a different question than
 * the one worth asking.  A table is band-limited for the top of the range it
 * serves, so it is missing the highest harmonics of a note near the bottom --
 * and at 1760Hz a sawtooth only has twelve harmonics at all, so losing the top
 * two is 16% of its energy and reads as 20dB in a sample-wise comparison.  Both
 * of those harmonics are above 19kHz.
 *
 * So the comparison is made over the audible band.  What is left is
 * interpolation error and any actual mistake, and a mistake does not stay
 * inside 15kHz.
 */
static double audible_snr_db(const std::vector<Type::scaled>& ref,
                             const std::vector<Type::scaled>& got,
                             double rate, double limit_hz) {
    const int n = 2048;
    const std::size_t off = ref.size()/2 - n/2;

    const std::vector<double> a = spectrum(ref, off, n);
    const std::vector<double> b = spectrum(got, off, n);

    const int top = std::min((int)a.size(), (int)(limit_hz * n / rate));

    double se = 0, sr = 0;
    for(int k = 1; k < top; k++) {
        const double d = b[k] - a[k];
        se += d*d;
        sr += a[k]*a[k];
    }

    return (se > 0) ? 10*std::log10(sr/se) : 1e9;
}

static void agreement() {
    std::cout << "a table against the sum it caches:\n";

    const double rate = 44100;
    const unsigned long n = 8192;

    wavetable_set set;

    for(auto w : {instrument::wave::sine, instrument::wave::saw,
                  instrument::wave::square, instrument::wave::triangle}) {
        instrument inst;
        inst.set_wave(w);

        for(double freq : {110.0, 440.0, 1760.0}) {
            const std::vector<Type::scaled> exact =
                render(inst, freq, n, rate, voice::quality::exact, 0);
            const std::vector<Type::scaled> fast =
                render(inst, freq, n, rate, voice::quality::fast, &set);

            const double db = audible_snr_db(exact, fast, rate, 15000.0);

            ok(instrument::name_of(w) + " at " + std::to_string((int)freq) + "Hz",
               db > 60.0, std::to_string((int)db) + "dB down below 15kHz");
        }
    }
}

static void no_aliasing() {
    std::cout << "\nstill band-limited:\n";

    // A table is band-limited for the top of the range it serves, so a note
    // anywhere in that range is at worst duller than exact -- never aliased.
    // The check is that nothing appears above Nyquist's mirror, which is what
    // aliasing would put there.
    const double rate = 44100;
    const unsigned long n = 4096;
    wavetable_set set;
    instrument inst;
    inst.set_wave(instrument::wave::saw);

    for(double freq : {4306.0, 8613.0}) {
        const std::vector<Type::scaled> fast =
            render(inst, freq, n, rate, voice::quality::fast, &set);

        // Aliasing has a signature of its own: a partial above Nyquist folds
        // back *down*, so it lands below the fundamental, where a band-limited
        // waveform has nothing at all.  That is the thing to look for, and
        // unlike counting off-harmonic energy it cannot be confused with the
        // banding this deliberately accepts.
        const int N = 2048;
        const std::vector<double> mag = spectrum(fast, 1024, N);

        double below = 0, above = 0;
        for(int k = 1; k < N/2; k++) {
            const double hz = k * rate / N;
            const double e = mag[k]*mag[k];

            // a little under, to allow for the fundamental's own smear
            if(hz < freq * 0.9) below += e; else above += e;
        }

        const double frac = (below + above > 0) ? below/(below+above) : 0;

        ok("saw at " + std::to_string((int)freq) + "Hz puts nothing below its fundamental",
           frac < 1e-4, std::to_string(frac));
    }
}

static void cost() {
    std::cout << "\ncost:\n";

    using namespace std::chrono;

    const double rate = 44100;
    const unsigned long n = (unsigned long)rate;   // one second
    const int voices = 64;

    instrument inst;
    inst.set_wave(instrument::wave::saw);

    wavetable_set set;
    set.prime(instrument::wave::saw, 100, 130, rate);

    double exact_s = 0, fast_s = 0;

    for(int pass = 0; pass < 2; pass++) {
        std::vector<voice> vs;
        for(int i = 0; i < voices; i++) {
            voice v(inst, 110.0, n, rate);
            if(pass) v.set_quality(voice::quality::fast, &set);
            vs.push_back(v);
        }

        std::vector<Type::scaled> out(n, 0);

        auto t0 = steady_clock::now();
        for(int i = 0; i < voices; i++) vs[i].render(out.data(), n, 1);
        auto t1 = steady_clock::now();

        (pass ? fast_s : exact_s) = duration<double>(t1 - t0).count();
    }

    std::cout << "  " << voices << " voices at 110Hz, one second of audio:\n"
              << "     exact " << std::fixed << std::setprecision(3) << exact_s
              << "s (" << std::setprecision(0) << exact_s*100 << "% of real time)\n"
              << "     fast  " << std::setprecision(3) << fast_s
              << "s (" << std::setprecision(1) << fast_s*100 << "% of real time)\n";

    ok("the table is much cheaper than the sum", fast_s * 10 < exact_s,
       std::to_string((int)(exact_s/fast_s)) + " times");

    ok("tables are built as they are needed, not all of them",
       set.size() > 0 && set.size() < 20, std::to_string(set.size()) + " built");
}

static void still_block_independent() {
    std::cout << "\nstill block size independent:\n";

    // The invariant from media_source_test has to survive the table, or an
    // offline render stops matching a live one.
    const double rate = 44100;
    const unsigned long total = 4096;

    wavetable_set set;
    instrument inst;
    inst.set_wave(instrument::wave::saw);

    std::vector<Type::scaled> whole(total, 0);
    {
        voice v(inst, 431.0, total, rate);
        v.set_quality(voice::quality::fast, &set);
        v.render(whole.data(), total, 1);
    }

    for(unsigned long block : {1UL, 7UL, 333UL}) {
        std::vector<Type::scaled> split(total, 0);
        voice v(inst, 431.0, total, rate);
        v.set_quality(voice::quality::fast, &set);

        unsigned long at = 0;
        while(at < total) {
            const unsigned long want = std::min(block, total - at);
            const unsigned long made = v.render(&split[at], want, 1);
            if(!made) break;
            at += made;
        }

        unsigned long differ = 0;
        for(unsigned long i = 0; i < total; i++)
            if(whole[i] != split[i]) ++differ;

        ok("blocks of " + std::to_string(block), differ == 0,
           differ ? (std::to_string(differ) + " differ") : "");
    }
}

int main() {
    agreement();
    no_aliasing();
    cost();
    still_block_independent();

    return failures ? 1 : 0;
}
