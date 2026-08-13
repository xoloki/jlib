// Verify the PCM notestream generates, for every format it supports.
//
// Silent by default -- pass --play for the clean formats, or --play-all for
// every one of them.  See audio_test.hh.
#include <iostream>
#include <string>

#include <jlib/media/notestream.hh>
#include <jlib/media/PortAudioSink.hh>
#include <jlib/media/Type.hh>

#include <jlib/sys/sys.hh>

#include "audio_test.hh"

using namespace jlib::media;
using namespace jlib::tests;

static int check(double freq, int format, int channels,
                 const std::string& tag, audio_mode mode)
{
    notestream note(freq);
    note.set_format(format);
    note.set_channels(channels);
    note.set_time(0.25);

    std::string pcm;
    jlib::sys::read(note, pcm);

    const std::string label =
        tag + " " + (channels > 1 ? "stereo" : "mono");

    if(!check_pcm(pcm, format, channels, label))
        return 1;

    if(should_play(mode, format)) {
        notestream again(freq);
        again.set_format(format);
        again.set_channels(channels);
        again.set_time(0.25);

        PortAudioSink dsp;
        dsp.play(again);
    }

    std::cout << label << ": " << pcm.size() << " bytes ok" << std::endl;
    return 0;
}

int main(int argc, char** argv) {
    const audio_mode mode = get_audio_mode(argc, argv);

    if(mode != AUDIO_SILENT && !PortAudioSink::have_output_device()) {
        std::cerr << "no audio output device, skipping" << std::endl;
        return 77;
    }

    int failures = 0;

    try {
        failures += check(110, Type::PCM_U8,      1, "Type::PCM_U8", mode);
        failures += check(220, Type::PCM_U8,      2, "Type::PCM_U8", mode);
        failures += check(110, Type::PCM_S8,      1, "Type::PCM_S8", mode);
        failures += check(440, Type::PCM_S16_LE,  1, "Type::PCM_S16_LE", mode);
        failures += check(880, Type::PCM_S16_LE,  2, "Type::PCM_S16_LE", mode);
        failures += check(440, Type::PCM_S16_BE,  1, "Type::PCM_S16_BE", mode);
        failures += check(440, Type::PCM_U16_LE,  1, "Type::PCM_U16_LE", mode);
        failures += check(440, Type::PCM_U16_BE,  1, "Type::PCM_U16_BE", mode);
        failures += check(440, Type::PCM_FLOAT32, 1, "Type::PCM_FLOAT32", mode);
        failures += check(880, Type::PCM_FLOAT32, 2, "Type::PCM_FLOAT32", mode);
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    return failures ? 1 : 0;
}
