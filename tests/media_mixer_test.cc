// The mixer: gain staging, the limiter, and the metering that makes both
// visible.
//
// The first section is the one that matters most, and it deliberately runs with
// the limiter switched off.  An earlier attempt at this problem used a dynamic
// compressor and made everything quiet -- which is what happens when gain is
// pulled down and nothing puts it back.  The failure is not subtle once it is
// measured, and it was never measured.  So: RMS as the voice count goes 1 to
// 64, with nothing downstream to rescue it.
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

/** A mix of n voices, all different pitches so they do not sum coherently. */
static std::shared_ptr<mixer> build(int n, unsigned long len, double rate,
                                    mixer::staging st, bool limit,
                                    double gain = 1.0) {
    auto m = std::make_shared<mixer>();
    m->set_staging(st);
    m->set_limiting(limit);

    instrument inst;

    for(int i = 0; i < n; i++) {
        // Spread over three octaves by the golden ratio, so no two voices land
        // on the same pitch and none are harmonically related.  This matters
        // more than it looks: an earlier version stepped by semitones modulo an
        // octave, which gave 64 voices only 24 distinct pitches.  Duplicates sum
        // in amplitude rather than in power, so the mix came out 1.6x too loud
        // and the staging appeared to be at fault when the pitches were.
        const double freq = 220.0 * std::pow(2.0, std::fmod(i * 0.6180339887, 3.0));
        m->add(std::make_shared<voice>(inst, freq, len, rate), gain);
    }

    return m;
}

static double rms_of(mixer& m, unsigned long len, unsigned int channels) {
    std::vector<Type::scaled> out(len * channels, 0);
    m.render(out.data(), len, channels);
    return m.rms();
}

static void staging_holds_the_level() {
    std::cout << "gain staging, limiter off:\n";

    const double rate = 44100;
    const unsigned long len = 8192;

    double first = 0;

    for(int n : {1, 2, 4, 16, 64}) {
        auto m = build(n, len, rate, mixer::staging::automatic, false);
        const double r = rms_of(*m, len, 1);

        if(n == 1) first = r;

        const double ratio = r / first;

        std::cout << "     " << std::setw(3) << n << " voices   rms "
                  << std::fixed << std::setprecision(4) << r
                  << "   " << std::setprecision(2) << ratio << "x the single voice\n";

        // Uncorrelated sources sum in power, so dividing by sqrt(N) holds the
        // level where one voice put it.  Measured 1.00 to 0.93 over this range;
        // the band is deliberately not much wider than that, since a band wide
        // enough to pass anything is what let the original go quiet unnoticed.
        ok(std::to_string(n) + " voices stay near the level of one",
           ratio > 0.85 && ratio < 1.15, std::to_string(ratio) + "x");
    }

    std::cout << "\n  and without staging, for comparison:\n";

    double bare_first = 0;
    for(int n : {1, 64}) {
        auto m = build(n, len, rate, mixer::staging::none, false);
        const double r = rms_of(*m, len, 1);

        if(n == 1) bare_first = r;
        else {
            const double ratio = r / bare_first;

            std::cout << "     64 voices unstaged   " << std::fixed
                      << std::setprecision(2) << ratio << "x, about sqrt(64) = 8\n";

            // If this did not grow, the staging above would be proving
            // nothing.  Asserting it against sqrt(N) rather than merely
            // "louder" makes it a check on the reasoning as well: the divisor
            // is 1/sqrt(N) precisely because this is what N voices do.
            ok("unstaged, 64 voices are about sqrt(64) louder",
               ratio > 6.0 && ratio < 10.0, std::to_string(ratio) + "x");
        }
    }
}

static void the_limiter() {
    std::cout << "\nlimiter:\n";

    const double rate = 44100;
    const unsigned long len = 8192;

    {
        // Genuinely below the ceiling -- four voices at a fifth of full gain
        // cannot reach it even if they align -- so the limiter must do nothing
        // whatsoever.  This is the compressor failure: pulling down what was
        // already fine, and never putting it back.
        auto m = build(4, len, rate, mixer::staging::automatic, false, 0.2);
        std::vector<Type::scaled> quiet(len, 0);
        m->render(quiet.data(), len, 1);
        const double without = m->rms();

        auto m2 = build(4, len, rate, mixer::staging::automatic, true, 0.2);
        std::vector<Type::scaled> limited(len, 0);
        m2->render(limited.data(), len, 1);
        const double with = m2->rms();

        ok("a quiet mix is untouched by the limiter",
           std::fabs(with - without) / without < 1e-6,
           std::to_string(with) + " against " + std::to_string(without));

        // One read only: the meters clear when read, so asking twice reports
        // the state after the first ask rather than the one being tested.
        const double did = m2->reduction();
        ok("and it says it did nothing", did > 0.999, std::to_string(did));
    }

    {
        // Deliberately far too hot: 32 voices, no staging, all gain up.
        auto m = build(32, len, rate, mixer::staging::none, true);
        m->set_ceiling(0.5);

        std::vector<Type::scaled> out(len, 0);
        m->render(out.data(), len, 1);

        double peak = 0;
        for(Type::scaled v : out) peak = std::max(peak, (double)std::fabs(v));

        // A little over the ceiling is expected at the very first sample: the
        // limiter cannot know a peak is coming before it arrives.
        ok("a hot mix is held near the ceiling", peak < 0.6,
           std::to_string(peak));

        const double worked = m->reduction();
        ok("and it says how hard it worked", worked < 0.5, std::to_string(worked));
    }
}

/**
 * Level over time, in 10ms windows.
 *
 * A window this short holds only a period or so of the 130Hz composite these
 * chords sit on, so its energy depends heavily on where it happened to land.
 * That would ruin an absolute measurement -- it once reported 7.8dB of swing
 * for a signal that has under 1dB, and a single unaccompanied sine showing the
 * same swing is what gave it away.  Here the same windows are used on both
 * renders and one is divided by the other, so the artefact is identical in
 * numerator and denominator and cancels; the unlimited case reading exactly
 * 0.00dB is the check on that.
 *
 * Short matters because the thing being measured is an 88ms wobble, and a 50ms
 * window averages a good half of it away: the same signal reads 0.50dB there
 * against 0.90dB measured on the limiter's gain directly.
 */
static std::vector<double> level(const std::vector<Type::scaled>& x) {
    const unsigned long w = 441, hop = 147;
    std::vector<double> e;
    for(unsigned long i = 0; i + w <= x.size(); i += hop) {
        double s = 0;
        for(unsigned long j = 0; j < w; j++) s += double(x[i+j]) * x[i+j];
        e.push_back(std::sqrt(s / w));
    }
    return e;
}

/**
 * How much the limiter modulates the level at a given rate, in dB.
 *
 * Dividing the limited level by the unlimited one gives the gain the limiter
 * applied, over time, without needing to reach inside it.
 */
static double tremolo(bool limiting, double release, double at_hz,
                      double headroom) {
    const double rate = 44100;
    const unsigned long N = 132300;                 // 3s

    // an equal tempered major triad, the case that provoked this
    const double C = 523.251;
    const double freqs[3] = { C, C * 1.2599210499, C * 1.4983070769 };

    std::vector<double> lvl[2];

    for(int pass = 0; pass < 2; pass++) {
        mixer m;
        m.set_staging(mixer::staging::automatic);
        m.set_limiting(pass == 1 && limiting);
        m.set_release(release);
        m.set_headroom(headroom);

        instrument inst;
        for(double f : freqs)
            m.add(std::make_shared<voice>(inst, f, N, rate));

        std::vector<Type::scaled> out(N, 0);
        m.render(out.data(), N, 1);
        lvl[pass] = level(out);
    }

    // the sustained middle, away from attack and release
    const std::size_t lo = lvl[0].size()/4, hi = lvl[0].size()*3/4;

    std::vector<double> gain;
    for(std::size_t i = lo; i < hi; i++) gain.push_back(lvl[1][i] / lvl[0][i]);

    double mean = 0;
    for(double g : gain) mean += g;
    mean /= gain.size();

    // one bin of a DFT, at the rate we care about
    const double hop_rate = rate / 147;
    double re = 0, im = 0;
    for(std::size_t i = 0; i < gain.size(); i++) {
        const double t = 2 * M_PI * at_hz * i / hop_rate;
        re += (gain[i] - mean) * std::cos(t);
        im -= (gain[i] - mean) * std::sin(t);
    }
    const double amp = 2 * std::sqrt(re*re + im*im) / gain.size();

    return 20 * std::log10((mean + amp) / (mean - amp));
}

/**
 * The limiter must not turn into a tremolo pedal.
 *
 * 1/sqrt(N) staging holds the RMS and says nothing about peaks, so three sine
 * voices at the default gain reach 0.666*sqrt(3) = 1.15 whenever they drift
 * into phase.  For an equal tempered triad that happens at 11.3Hz -- the gap
 * between its two difference tones, 136.0 and 124.7Hz -- so the limiter fires
 * at 11.3Hz, and if it lets go quickly enough to follow, the chord audibly
 * wobbles.  A sawtooth hides it because its peak is high and steady.
 *
 * Headroom now keeps the limiter off a chord entirely, so this cannot arise as
 * shipped.  The rest of the measurements deliberately switch the headroom off,
 * because a release short enough to wobble is still a bug wherever the limiter
 * does engage, and a test that only exercised the shipping configuration would
 * stop watching for it.
 */
static void not_a_tremolo_pedal() {
    std::cout << "\nthe limiter is not a tremolo pedal:\n";

    mixer defaults;

    // Two independent defences, so they are measured separately.  The headroom
    // stops the limiter engaging on a chord at all; the release stops it
    // wobbling when something does engage it.  Either alone is enough here,
    // which is why the second is measured with the first switched off.
    const double both = tremolo(true, defaults.get_release(), 11.268,
                                defaults.get_headroom());
    const double slow = tremolo(true, 0.25, 11.268, 0.0);
    const double fast = tremolo(true, 0.02, 11.268, 0.0);
    const double none = tremolo(false, 0.25, 11.268, 0.0);

    std::cout << "     as shipped              " << std::fixed << std::setprecision(2)
              << both << " dB at 11.3 Hz\n"
              << "     no headroom, 250ms      " << slow << " dB\n"
              << "     no headroom,  20ms      " << fast << " dB\n"
              << "     no headroom, no limiter " << none << " dB\n";

    ok("as shipped the chord does not wobble at all", std::fabs(both) < 0.02,
       std::to_string(both) + " dB");

    ok("and the release alone would have handled it", slow < 0.35,
       std::to_string(slow) + " dB");

    // Without this the test could pass by measuring nothing at all.
    ok("and the measurement can see a release that would not", fast > 0.7,
       std::to_string(fast) + " dB");

    ok("with no limiter there is nothing to see", std::fabs(none) < 0.02,
       std::to_string(none) + " dB");
}

/**
 * Level must not depend on how many voices are sounding.
 *
 * This is what staging is for and it is the thing the limiter was quietly
 * taking away.  Holding the RMS says nothing about peaks, so a chord went over
 * the ceiling where a single note did not, the limiter engaged on one and not
 * the other, and the chord came out a dB quieter -- the inconsistency arriving
 * by exactly the mechanism meant to prevent it.
 *
 * With headroom the limiter stays out of the path and the levels match, which
 * is asserted here by requiring that it reports having done nothing at all.
 */
static void level_does_not_depend_on_voice_count() {
    std::cout << "\nlevel against voice count, as shipped:\n";

    const double rate = 44100;
    const unsigned long len = 8192;

    double first = 0;

    for(int n : {1, 2, 3, 4}) {
        auto m = build(n, len, rate, mixer::staging::automatic, true);
        std::vector<Type::scaled> out(len, 0);
        m->render(out.data(), len, 1);

        const double r = m->rms();
        const double red = m->reduction();
        if(n == 1) first = r;

        double peak = 0;
        for(Type::scaled v : out) peak = std::max(peak, (double)std::fabs(v));

        std::cout << "     " << n << " voices   rms " << std::fixed
                  << std::setprecision(4) << r << "   peak " << std::setprecision(3)
                  << peak << "   limiter " << std::setprecision(2)
                  << 20*std::log10(red) << " dB\n";

        ok(std::to_string(n) + " voices: the limiter stays out of it", red > 0.9999,
           std::to_string(red));

        ok(std::to_string(n) + " voices: and the level matches one voice",
           std::fabs(r/first - 1.0) < 0.02, std::to_string(r/first) + "x");
    }

    // What this does NOT establish.  Three dB of headroom covers a handful of
    // voices, not any number of them: the peak plateaus around 5dB over the
    // ceiling, so past roughly six voices the limiter starts engaging again and
    // the level sags by up to a dB.  That is the intended division of labour --
    // headroom for the sparse case where inconsistency would be audible, the
    // limiter for the dense case where it is masked -- but it is a balance and
    // not a guarantee, and set_headroom(5) buys the guarantee for 2dB.
    auto dense = build(32, len, rate, mixer::staging::automatic, true);
    std::vector<Type::scaled> out(len, 0);
    dense->render(out.data(), len, 1);
    std::cout << "     32 voices  rms " << std::fixed << std::setprecision(4)
              << dense->rms() << "   limiter " << std::setprecision(2)
              << 20*std::log10(dense->reduction()) << " dB   (headroom is a"
              << " balance, not a guarantee)\n";
}

static void metering() {
    std::cout << "\nmetering:\n";

    const double rate = 44100;
    const unsigned long len = 4096;

    auto m = build(4, len, rate, mixer::staging::automatic, true);

    std::vector<Type::scaled> out(len, 0);
    m->render(out.data(), len, 1);

    double peak = 0;
    for(Type::scaled v : out) peak = std::max(peak, (double)std::fabs(v));

    const Type::scaled reported = m->peak();

    ok("peak matches what came out",
       std::fabs(reported - peak) < 1e-6,
       std::to_string(reported) + " against " + std::to_string(peak));

    ok("and reading it clears it", m->peak() == 0);
}

static void housekeeping() {
    std::cout << "\nchildren:\n";

    const double rate = 44100;
    instrument inst;

    {
        auto m = std::make_shared<mixer>();
        m->add(std::make_shared<voice>(inst, 220, 128, rate));
        m->add(std::make_shared<voice>(inst, 330, 4096, rate));

        std::vector<Type::scaled> out(4096, 0);
        m->render(out.data(), 256, 1);          // long enough to finish the first

        ok("a finished child stays until pruned", m->size() == 2);
        m->prune();
        ok("and goes when it is", m->size() == 1);
    }

    {
        // the cap sounds the loudest, since that is what would be missed least
        auto m = std::make_shared<mixer>();
        m->set_limiting(false);   // else both sides clamp to the ceiling and
                                  // the comparison measures nothing
        for(int i = 0; i < 8; i++)
            m->add(std::make_shared<voice>(inst, 220.0 * (i+1), 512, rate),
                   0.1 * (i + 1));

        m->set_max_voices(3);

        std::vector<Type::scaled> capped(512, 0);
        m->render(capped.data(), 512, 1);
        const Type::scaled with_cap = m->peak();

        m->reset();
        m->set_max_voices(0);

        std::vector<Type::scaled> all(512, 0);
        m->render(all.data(), 512, 1);
        const Type::scaled without = m->peak();

        ok("a voice cap sounds fewer of them", with_cap < without,
           std::to_string(with_cap) + " against " + std::to_string(without));
    }
}

static void block_size_independence() {
    std::cout << "\nstill block size independent:\n";

    const double rate = 44100;
    const unsigned long total = 4096;

    for(auto st : {mixer::staging::none, mixer::staging::automatic}) {
        for(bool limit : {false, true}) {
            auto whole_m = build(8, total, rate, st, limit);
            std::vector<Type::scaled> whole(total, 0);
            whole_m->render(whole.data(), total, 1);

            unsigned long worst = 0;

            for(unsigned long block : {1UL, 7UL, 333UL}) {
                auto m = build(8, total, rate, st, limit);
                std::vector<Type::scaled> split(total, 0);

                unsigned long at = 0;
                while(at < total) {
                    const unsigned long want = std::min(block, total - at);
                    const unsigned long made = m->render(&split[at], want, 1);
                    if(!made) break;
                    at += made;
                }

                for(unsigned long i = 0; i < total; i++)
                    if(whole[i] != split[i]) ++worst;
            }

            ok(std::string(st == mixer::staging::none ? "unstaged" : "staged") +
               (limit ? ", limited" : ", unlimited"),
               worst == 0, worst ? (std::to_string(worst) + " differ") : "");
        }
    }
}

int main() {
    staging_holds_the_level();
    the_limiter();
    not_a_tremolo_pedal();
    level_does_not_depend_on_voice_count();
    metering();
    housekeeping();
    block_size_independence();

    return failures ? 1 : 0;
}
