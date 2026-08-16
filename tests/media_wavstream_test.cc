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
