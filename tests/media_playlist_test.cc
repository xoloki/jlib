// PlayList, which has never had a test.
//
// render() is reached through the format dispatcher in PlayList.cc, so it has
// always been compiled; nothing has ever called it.  Two things were broken in
// it, and neither is the sort a compiler finds.
#include <jlib/media/PlayList.hh>
#include <jlib/media/WavFile.hh>
#include <jlib/media/instrument.hh>
#include <jlib/media/voice.hh>
#include <jlib/media/wavstream.hh>

#include <cmath>
#include <complex>
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

/** A short note at a chosen level, as a WAV on disk. */
static void make_wav(const std::string& path, double gain, double seconds) {
    const double rate = 44100;
    const unsigned long frames = static_cast<unsigned long>(rate * seconds);

    instrument inst;
    inst.set_gain(gain);
    voice v(inst, 440, frames, rate);

    std::vector<Type::scaled> mix(frames, 0);
    v.render(mix.data(), frames, 1);

    std::string pcm;
    pcm.reserve(frames * 2);
    for(Type::scaled s : mix) {
        const double c = s > 1 ? 1 : (s < -1 ? -1 : s);
        const short q = static_cast<short>(std::lround(c * 32767));
        pcm.push_back(static_cast<char>(q & 0xff));
        pcm.push_back(static_cast<char>((q >> 8) & 0xff));
    }

    WavFile out;
    out.set_format(Type::PCM_S16_LE);
    out.set_channels(1);
    out.set_samples_per_sec(static_cast<int>(rate));
    out.set_bits_per_sample(16);
    out.set_pcm(pcm);
    out.save(path);
}

static std::vector<float> as_floats(const std::string& data) {
    std::vector<float> v(data.size() / sizeof(float));
    if(!v.empty())
        std::memcpy(v.data(), data.data(), v.size() * sizeof(float));
    return v;
}

/** Loudest sample in one tick's worth of the render. */
static double tick_peak(const std::vector<float>& v, int tick, int per_tick) {
    double p = 0;
    for(int i = tick * per_tick; i < (tick+1) * per_tick && i < (int)v.size(); i++)
        p = std::max(p, (double)std::fabs(v[i]));
    return p;
}

static PlayList make_list() {
    PlayList list(1, "test");
    list.set_bpm(120);
    list.set_measure(4);       // 480 ticks a minute, so 8 ticks is one second
    list.set_width(8);
    return list;
}

static void hits_land_on_their_ticks() {
    std::cout << "hits land where the pattern says:\n";

    make_wav("playlist_test_a.wav", 0.6, 0.05);
    wavstream s("playlist_test_a.wav");

    PlayList list = make_list();
    Pattern p(1, "p");
    p.push_back(Roll(1, &s, "roll", "playlist_test_a.wav", "10100001"));

    PlayList::slice_type slice;
    slice.push_back(p);

    const std::vector<float> v = as_floats(list.render(Type::PCM_FLOAT32, slice));

    ok("renders one second", v.size() == 44100, std::to_string(v.size()));
    if(v.size() != 44100) return;

    const int per_tick = 44100 / 8;

    for(int t = 0; t < 8; t++) {
        const bool want = (t == 0 || t == 2 || t == 7);
        const double p2 = tick_peak(v, t, per_tick);

        std::cout << "     tick " << t << "  peak " << std::fixed
                  << std::setprecision(4) << p2
                  << (want ? "   (struck)" : "") << "\n";

        ok(std::string("tick ") + std::to_string(t) +
           (want ? " sounds" : " is silent"),
           want ? (p2 > 0.05) : (p2 < 0.001), std::to_string(p2));
    }
}

/**
 * The regression that matters, and the reason this file exists.
 *
 * render() used to take the peak over the whole pattern and, if it came out
 * above one, divide every sample by it.  That is not a limiter and not a
 * compressor: it is non-causal.  A loud hit at the end of a bar reached back
 * and quieted the beginning of it, and nothing made up the difference.
 *
 * So: sound a quiet hit early and a loud one late, and compare the early hit
 * against the same pattern with nothing loud in it.  It must not have moved.
 * Under the old code it moved by whatever the loud hit happened to peak at.
 */
static void a_loud_hit_late_does_not_reach_back() {
    std::cout << "\na loud hit late must not change an earlier quiet one:\n";

    make_wav("playlist_test_quiet.wav", 0.25, 0.05);
    make_wav("playlist_test_loud.wav",  1.0,  0.05);

    const int per_tick = 44100 / 8;
    double early[2] = {0, 0};

    for(int pass = 0; pass < 2; pass++) {
        // pass 0: both rolls quiet.  pass 1: the late roll is loud.
        // Two rolls strike together late, so their sum is over full scale --
        // a single clip cannot be, having come off disk as 16-bit PCM, and the
        // old normalize only triggered strictly above one.  Both passes carry
        // the same three rolls so the gain staging divisor is identical and
        // the only thing that changes is how loud the late pair is.
        wavstream a("playlist_test_quiet.wav");
        wavstream b(pass ? "playlist_test_loud.wav" : "playlist_test_quiet.wav");
        wavstream c(pass ? "playlist_test_loud.wav" : "playlist_test_quiet.wav");

        PlayList list = make_list();
        Pattern p(1, "p");
        p.push_back(Roll(1, &a, "early", "a", "10000000"));
        p.push_back(Roll(2, &b, "late",  "b", "00000010"));
        p.push_back(Roll(3, &c, "also",  "c", "00000010"));

        PlayList::slice_type slice;
        slice.push_back(p);

        const std::vector<float> v = as_floats(list.render(Type::PCM_FLOAT32, slice));
        if(v.size() != 44100) { ok("renders", false); return; }

        early[pass] = tick_peak(v, 0, per_tick);

        std::cout << "     " << (pass ? "with a loud hit at tick 6" : "all quiet")
                  << "   early hit peaks at " << std::fixed << std::setprecision(4)
                  << early[pass] << "   late hit " << tick_peak(v, 6, per_tick) << "\n";
    }

    const double ratio = early[1] / early[0];
    ok("the early hit is untouched", std::fabs(ratio - 1.0) < 0.001,
       std::to_string(ratio) + "x");
}

/**
 * A hit near the end runs past it, and has always wrapped to the start so that
 * a pattern loops seamlessly.  Kept in the port, so it is worth pinning.
 */
static void the_tail_wraps() {
    std::cout << "\na hit that overruns the bar wraps to the front:\n";

    // Half a second of sample struck on the last of eight ticks, so most of it
    // lands past the end of a one second pattern.
    make_wav("playlist_test_long.wav", 0.6, 0.5);
    wavstream s("playlist_test_long.wav");

    PlayList list = make_list();
    Pattern p(1, "p");
    p.push_back(Roll(1, &s, "long", "long", "00000001"));

    PlayList::slice_type slice;
    slice.push_back(p);

    const std::vector<float> v = as_floats(list.render(Type::PCM_FLOAT32, slice));
    ok("renders one second", v.size() == 44100, std::to_string(v.size()));
    if(v.size() != 44100) return;

    const int per_tick = 44100 / 8;

    ok("the tail is audible at the start of the bar",
       tick_peak(v, 0, per_tick) > 0.05,
       std::to_string(tick_peak(v, 0, per_tick)));

    ok("and the hit itself is still there",
       tick_peak(v, 7, per_tick) > 0.05,
       std::to_string(tick_peak(v, 7, per_tick)));
}

/** How much of one frequency sits in a stretch of the render. */
static double energy_at(const std::vector<float>& v, int from, int count, double f) {
    std::complex<double> a = 0;
    for(int i = from; i < from + count && i < (int)v.size(); i++)
        a += double(v[i]) * std::exp(std::complex<double>(0, -2*M_PI*f*i/44100));
    return std::abs(a) / count;
}

/**
 * A roll can sound an instrument instead of a recording.
 *
 * Which is the whole of the timeline claim: a pattern names a note the same way
 * jnote and jmelody do, it becomes a voice rather than a sampler, and nothing
 * from the mixer down can tell the difference.
 */
static void notes_as_well_as_recordings() {
    std::cout << "\nrolls that sound notes:\n";

    const int per_tick = 44100 / 8;

    {
        PlayList list = make_list();
        Pattern p(1, "p");
        // a tenth of a second, so a note fits inside its eighth-second tick
        p.push_back(Roll(1, "A@2:0.1", "note", "10001000"));

        PlayList::slice_type slice;
        slice.push_back(p);

        const std::vector<float> v = as_floats(list.render(Type::PCM_FLOAT32, slice));
        ok("renders one second", v.size() == 44100, std::to_string(v.size()));
        if(v.size() != 44100) return;

        for(int t = 0; t < 8; t++) {
            const bool want = (t == 0 || t == 4);
            const double p2 = tick_peak(v, t, per_tick);
            ok(std::string("tick ") + std::to_string(t) +
               (want ? " sounds" : " is silent"),
               want ? (p2 > 0.05) : (p2 < 0.001), std::to_string(p2));
        }

        // and it is the pitch that was asked for, not merely a noise
        const double at220 = energy_at(v, 0, per_tick, 220);
        const double at330 = energy_at(v, 0, per_tick, 330);
        ok("and it is an A at 220Hz", at220 > at330 * 20,
           std::to_string(at220) + " at 220 against " + std::to_string(at330) + " at 330");
    }

    {
        // the waveform override survives the trip through the pattern
        PlayList list = make_list();
        Pattern sine(1, "sine"), saw(2, "saw");
        sine.push_back(Roll(1, "A@2:0.1", "n", "10000000"));
        saw.push_back(Roll(1, "A@2:0.1/saw", "n", "10000000"));

        PlayList::slice_type a, b;
        a.push_back(sine);
        b.push_back(saw);

        const std::vector<float> vs = as_floats(list.render(Type::PCM_FLOAT32, a));
        const std::vector<float> vw = as_floats(list.render(Type::PCM_FLOAT32, b));

        // a sawtooth has a second harmonic and a sine has essentially none
        const double sine440 = energy_at(vs, 0, per_tick, 440);
        const double saw440  = energy_at(vw, 0, per_tick, 440);

        ok("/saw reaches the voice", saw440 > sine440 * 20,
           std::to_string(saw440) + " against " + std::to_string(sine440) + " at 440");
    }

    {
        PlayList list = make_list();
        bool threw = false;
        try { Roll(1, "H@9", "bad", "10000000"); }
        catch(std::exception&) { threw = true; }
        ok("a note it cannot read throws when the roll is made", threw);
    }
}

/**
 * The two kinds of roll in one pattern.
 *
 * Not asserted by comparing against the two rendered separately and added:
 * automatic staging divides by the number of rolls, so one pattern of two is
 * deliberately not the sum of two patterns of one.  What is checked instead is
 * that each lands on its own beat and sounds like itself, with the pattern
 * holding one of each.
 */
static void a_recording_and_a_note_together() {
    std::cout << "\na recording and a note in one pattern:\n";

    make_wav("playlist_test_a.wav", 0.6, 0.05);

    const int per_tick = 44100 / 8;

    std::vector<float> both;

    {
        wavstream s("playlist_test_a.wav");
        PlayList list = make_list();
        Pattern p(1, "p");
        p.push_back(Roll(1, &s, "drum", "a", "10000000"));
        p.push_back(Roll(2, "A@2:0.1", "note", "00001000"));

        PlayList::slice_type slice;
        slice.push_back(p);
        both = as_floats(list.render(Type::PCM_FLOAT32, slice));
    }

    ok("renders one second", both.size() == 44100, std::to_string(both.size()));
    if(both.size() != 44100) return;

    ok("the recording is on its beat", tick_peak(both, 0, per_tick) > 0.05,
       std::to_string(tick_peak(both, 0, per_tick)));
    ok("and the note is on its own", tick_peak(both, 4, per_tick) > 0.05,
       std::to_string(tick_peak(both, 4, per_tick)));
    ok("with silence between them", tick_peak(both, 2, per_tick) < 0.001,
       std::to_string(tick_peak(both, 2, per_tick)));

    ok("the note is a 220Hz A",
       energy_at(both, 4*per_tick, per_tick, 220) >
       energy_at(both, 4*per_tick, per_tick, 330) * 20);
}

static void nothing_to_play() {
    std::cout << "\ndegenerate patterns:\n";

    {
        PlayList list = make_list();
        PlayList::slice_type slice;
        ok("an empty slice renders silence, not a crash",
           list.render(Type::PCM_FLOAT32, slice).size() == 44100 * sizeof(float));
    }

    {
        // A Roll with no stream.  This used to be undefined: the default
        // constructor left m_stream uninitialized, so the null check that
        // guards this read whatever was on the stack.
        PlayList list = make_list();
        Pattern p(1, "p");
        p.push_back(Roll());

        PlayList::slice_type slice;
        slice.push_back(p);

        ok("a roll with no stream is skipped",
           list.render(Type::PCM_FLOAT32, slice).size() == 44100 * sizeof(float));
    }
}

int main() {
    std::cout << std::unitbuf;

    try {
        hits_land_on_their_ticks();
        a_loud_hit_late_does_not_reach_back();
        the_tail_wraps();
        notes_as_well_as_recordings();
        a_recording_and_a_note_together();
        nothing_to_play();
    }
    catch(std::exception& e) {
        std::cerr << "playlist test: " << e.what() << std::endl;
        return 1;
    }

    for(const char* f : {"playlist_test_a.wav", "playlist_test_quiet.wav",
                         "playlist_test_loud.wav", "playlist_test_long.wav"})
        std::remove(f);

    return failures ? 1 : 0;
}
