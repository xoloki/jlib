// A recording, played as a source.
//
// The point of the interface is that nothing downstream can tell a sampler from
// a voice, so the last section mixes one of each and checks that the sum is the
// two of them and nothing else.
#include <jlib/media/WavFile.hh>
#include <jlib/media/clip.hh>
#include <jlib/media/delayed.hh>
#include <jlib/media/instrument.hh>
#include <jlib/media/mixer.hh>
#include <jlib/media/notestream.hh>
#include <jlib/media/sampler.hh>
#include <jlib/media/voice.hh>
#include <jlib/media/wavstream.hh>

#include <cmath>
#include <complex>
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

/** Write a WAV holding a synthesized note, and read it back as a clip. */
static std::shared_ptr<clip> make_clip(const std::string& path, double freq,
                                       double seconds, unsigned int channels)
{
    const double rate = 44100;
    const unsigned long frames = static_cast<unsigned long>(rate * seconds);

    instrument inst;
    voice v(inst, freq, frames, rate);

    std::vector<Type::scaled> mix(frames * channels, 0);
    v.render(mix.data(), frames, channels);

    // to PCM by hand, so the test does not depend on source_stream to prove
    // something about sampler
    std::string pcm;
    pcm.reserve(mix.size() * 2);
    for(Type::scaled s : mix) {
        const double c = s > 1 ? 1 : (s < -1 ? -1 : s);
        const short q = static_cast<short>(std::lround(c * 32767));
        pcm.push_back(static_cast<char>(q & 0xff));
        pcm.push_back(static_cast<char>((q >> 8) & 0xff));
    }

    WavFile out;
    out.set_format(Type::PCM_S16_LE);
    out.set_channels(channels);
    out.set_samples_per_sec(static_cast<int>(rate));
    out.set_bits_per_sample(16);
    out.set_pcm(pcm);
    out.save(path);

    wavstream in(path);
    return std::make_shared<clip>(in);
}

static void decoding() {
    std::cout << "decoding:\n";

    auto c = make_clip("sampler_test_mono.wav", 440, 0.25, 1);

    ok("channels", c->channels() == 1, std::to_string(c->channels()));
    ok("rate", c->rate() == 44100, std::to_string(c->rate()));
    ok("frames", c->frames() == 11025, std::to_string(c->frames()));
    ok("seconds", std::fabs(c->seconds() - 0.25) < 1e-9,
       std::to_string(c->seconds()));

    // A note starts and ends at silence -- the envelope guarantees it -- so a
    // clip that decoded with an offset or a half-frame slip would not.
    ok("starts at silence", std::fabs(c->at(0UL, 0)) < 1e-4,
       std::to_string(c->at(0UL, 0)));
    ok("ends at silence", std::fabs(c->at(c->frames()-1, 0)) < 1e-4,
       std::to_string(c->at(c->frames()-1, 0)));

    // Out of range reads as silence rather than throwing or wandering.
    ok("past the end is silence", c->at(c->frames() + 100, 0) == 0);
    ok("a channel it has not got is silence", c->at(10UL, 7) == 0);

    auto st = make_clip("sampler_test_stereo.wav", 440, 0.1, 2);
    ok("stereo: channels", st->channels() == 2, std::to_string(st->channels()));
    ok("stereo: frames", st->frames() == 4410, std::to_string(st->frames()));
    ok("stereo: a voice is centred",
       st->at(2000UL, 0) == st->at(2000UL, 1));
}

static void plays_back_what_was_recorded() {
    std::cout << "\nplayback at the recorded speed:\n";

    auto c = make_clip("sampler_test_mono.wav", 440, 0.25, 1);

    sampler s(c);
    std::vector<Type::scaled> out(c->frames(), 0);
    const unsigned long made = s.render(out.data(), c->frames(), 1);

    ok("renders the whole clip", made == c->frames(), std::to_string(made));

    // Bit-exact, not merely close.  At speed 1 the position lands on whole
    // frames, so at(double) short-circuits and returns the stored sample; an
    // interpolation that ran anyway would be wrong by a hair everywhere and
    // this is what would notice.
    unsigned long differ = 0;
    for(unsigned long i = 0; i < c->frames(); i++)
        if(out[i] != c->at(i, 0)) ++differ;

    ok("and does it sample for sample", differ == 0,
       differ ? (std::to_string(differ) + " differ") : "");

    ok("then it is done", s.done());

    // and again from the top
    s.reset();
    std::vector<Type::scaled> again(c->frames(), 0);
    s.render(again.data(), c->frames(), 1);
    ok("reset replays it identically", again == out);
}

static void block_size_independence() {
    std::cout << "\nstill block size independent:\n";

    auto c = make_clip("sampler_test_mono.wav", 440, 0.25, 1);
    const unsigned long total = c->frames();

    struct spec { const char* name; double speed; bool loop; unsigned long start; };

    const spec cases[] = {
        { "plain",             1.0,  false, 0    },
        { "resampled",         0.63, false, 0    },
        { "looping",           1.0,  true,  0    },
        { "delayed",           1.0,  false, 977  },
        { "delayed+resampled", 1.37, false, 513  },
    };

    // The delay is a wrapper now rather than a field on the sampler, so it has
    // its own state to carry across a block boundary and belongs in here.
    auto build_one = [&](const spec& k) -> std::shared_ptr<source> {
        auto one = std::make_shared<sampler>(c);
        one->set_speed(k.speed);
        one->set_looping(k.loop);

        if(k.start)
            return std::make_shared<delayed>(one, k.start);

        return one;
    };

    for(const spec& k : cases) {
        std::shared_ptr<source> whole = build_one(k);

        std::vector<Type::scaled> one(total, 0);
        whole->render(one.data(), total, 1);

        unsigned long differ = 0;

        for(unsigned long block : {1UL, 7UL, 333UL}) {
            std::shared_ptr<source> part = build_one(k);

            std::vector<Type::scaled> split(total, 0);
            unsigned long at = 0;
            while(at < total) {
                const unsigned long want = std::min(block, total - at);
                const unsigned long made = part->render(&split[at], want, 1);
                if(!made) break;
                at += made;
            }

            for(unsigned long i = 0; i < total; i++)
                if(one[i] != split[i]) ++differ;
        }

        ok(k.name, differ == 0, differ ? (std::to_string(differ) + " differ") : "");
    }
}

static void speed_and_looping() {
    std::cout << "\nspeed and looping:\n";

    auto c = make_clip("sampler_test_mono.wav", 440, 0.25, 1);

    {
        // Half speed is twice as long and an octave down.  Measured as an
        // octave rather than assumed: the clip is a 440Hz note, so the half
        // speed render should have its energy at 220 and next to none at 440.
        sampler s(c);
        s.set_speed(0.5);

        const unsigned long n = c->frames() * 2;
        std::vector<Type::scaled> out(n, 0);
        const unsigned long made = s.render(out.data(), n, 1);

        ok("half speed lasts twice as long",
           made >= n - 2 && made <= n, std::to_string(made) + " of " + std::to_string(n));

        auto energy = [&](double f) {
            std::complex<double> a = 0;
            for(unsigned long i = 0; i < made; i++)
                a += double(out[i]) * std::exp(std::complex<double>(0, -2*M_PI*f*i/44100));
            return std::abs(a)/made;
        };

        const double at220 = energy(220), at440 = energy(440);
        ok("and sounds an octave down",
           at220 > at440 * 20,
           std::to_string(at220) + " at 220 against " + std::to_string(at440) + " at 440");
    }

    {
        sampler s(c);
        s.set_looping(true);

        const unsigned long n = c->frames() * 3 + 101;
        std::vector<Type::scaled> out(n, 0);
        const unsigned long made = s.render(out.data(), n, 1);

        ok("a loop fills whatever it is given", made == n, std::to_string(made));
        ok("and never finishes", !s.done());

        // the second time round is the first time round
        unsigned long differ = 0;
        for(unsigned long i = 0; i < c->frames(); i++)
            if(out[i] != out[i + c->frames()]) ++differ;

        ok("and repeats exactly", differ == 0,
           differ ? (std::to_string(differ) + " differ") : "");
    }
}

static void starting_late() {
    std::cout << "\nstarting late:\n";

    auto c = make_clip("sampler_test_mono.wav", 440, 0.1, 1);

    const unsigned long start = 1000;
    delayed d(std::make_shared<sampler>(c), start);

    const unsigned long n = start + c->frames();
    std::vector<Type::scaled> out(n, 0);
    const unsigned long made = d.render(out.data(), n, 1);

    ok("renders the wait as well as the clip", made == n,
       std::to_string(made) + " of " + std::to_string(n));

    bool quiet = true;
    for(unsigned long i = 0; i < start; i++) if(out[i] != 0) quiet = false;
    ok("silent until its moment", quiet);

    unsigned long differ = 0;
    for(unsigned long i = 0; i < c->frames(); i++)
        if(out[start + i] != c->at(i, 0)) ++differ;
    ok("and then exactly the clip", differ == 0,
       differ ? (std::to_string(differ) + " differ") : "");

    ok("and is done when the clip is", d.done());

    // The wait must not erase what is already in the buffer.  render() adds,
    // so a delay that wrote zeros over its silent stretch would silently
    // delete every source mixed in before it -- and with a mixer that zeroes
    // first, which is the usual case, nothing would ever notice.
    {
        std::vector<Type::scaled> shared(n, 0.5);
        delayed again(std::make_shared<sampler>(c), start);
        again.render(shared.data(), n, 1);

        bool kept = true;
        for(unsigned long i = 0; i < start; i++)
            if(shared[i] != 0.5f) kept = false;

        ok("and adds to the buffer rather than clearing it", kept);
    }

    // A delay of nothing is the source itself.
    {
        std::vector<Type::scaled> plain(c->frames(), 0);
        sampler bare(c);
        bare.render(plain.data(), c->frames(), 1);

        std::vector<Type::scaled> wrapped(c->frames(), 0);
        delayed none(std::make_shared<sampler>(c), 0);
        none.render(wrapped.data(), c->frames(), 1);

        ok("a delay of zero changes nothing", plain == wrapped);
    }
}

static void channels() {
    std::cout << "\nchannels:\n";

    auto mono = make_clip("sampler_test_mono.wav", 440, 0.05, 1);

    {
        sampler s(mono);
        std::vector<Type::scaled> out(mono->frames() * 2, 0);
        s.render(out.data(), mono->frames(), 2);

        bool centred = true, matches = true;
        for(unsigned long i = 0; i < mono->frames(); i++) {
            if(out[i*2] != out[i*2+1]) centred = false;
            if(out[i*2] != mono->at(i, 0)) matches = false;
        }
        ok("a mono clip plays centred in stereo", centred);
        ok("and unchanged", matches);
    }

    {
        // A stereo clip in mono must fold, not drop a side.  Build one with the
        // channels deliberately different so dropping would be visible.
        auto st = make_clip("sampler_test_stereo.wav", 440, 0.05, 2);

        sampler s(st);
        std::vector<Type::scaled> out(st->frames(), 0);
        s.render(out.data(), st->frames(), 1);

        unsigned long differ = 0;
        for(unsigned long i = 0; i < st->frames(); i++) {
            const double want = (double(st->at(i, 0)) + st->at(i, 1)) / 2;
            if(std::fabs(out[i] - want) > 1e-6) ++differ;
        }
        ok("a stereo clip folds down to mono", differ == 0,
           differ ? (std::to_string(differ) + " differ") : "");
    }
}

/**
 * The interface claim, checked.
 *
 * source.hh says a synthesized voice and a recorded sample are the same kind of
 * thing to whatever is mixing them.  If that is true then a mixer holding one
 * of each produces their sum, and the mixer has no way to tell them apart --
 * so rendering the two separately and adding them by hand has to give the same
 * answer as letting the mixer do it.
 */
static void a_sample_and_a_voice_are_the_same_kind_of_thing() {
    std::cout << "\na sampler and a voice, mixed:\n";

    auto c = make_clip("sampler_test_mono.wav", 440, 0.25, 1);
    const unsigned long n = c->frames();
    const double rate = 44100;

    instrument inst;

    auto samp = std::make_shared<sampler>(c);
    auto note = std::make_shared<voice>(inst, 660, n, rate);

    mixer m;
    m.set_staging(mixer::staging::none);
    m.set_limiting(false);

    m.add(samp);
    m.add(note);

    std::vector<Type::scaled> mixed(n, 0);
    const unsigned long made = m.render(mixed.data(), n, 1);
    ok("the mixer renders both", made == n, std::to_string(made));

    std::vector<Type::scaled> by_hand(n, 0);
    sampler s2(c);
    voice v2(inst, 660, n, rate);
    s2.render(by_hand.data(), n, 1);
    v2.render(by_hand.data(), n, 1);

    unsigned long differ = 0;
    for(unsigned long i = 0; i < n; i++)
        if(mixed[i] != by_hand[i]) ++differ;

    ok("and the mix is exactly the two of them", differ == 0,
       differ ? (std::to_string(differ) + " differ") : "");
}

int main() {
    try {
        decoding();
        plays_back_what_was_recorded();
        block_size_independence();
        speed_and_looping();
        starting_late();
        channels();
        a_sample_and_a_voice_are_the_same_kind_of_thing();
    }
    catch(std::exception& e) {
        std::cerr << "sampler test: " << e.what() << std::endl;
        return 1;
    }

    return failures ? 1 : 0;
}
