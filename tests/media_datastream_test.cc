#include <iostream>
#include <fstream>

#include <unistd.h>

#include <cmath>

#include <jlib/media/datastream.hh>
#include <jlib/media/notestream.hh>
#include <jlib/media/PortAudioSink.hh>

#include "audio_test.hh"

#include <jlib/sys/sys.hh>

const long double 	PI = 3.14159265358979323846264338;



int main(int argc, char** argv) {
    using namespace jlib::media;
    using namespace jlib::tests;

    const audio_mode mode = get_audio_mode(argc, argv);

    if(mode != AUDIO_SILENT && !PortAudioSink::have_output_device()) {
        std::cerr << "no audio output device, skipping" << std::endl;
        return 77;
    }

    try {
        const int format = Type::PCM_U8;

        notestream note(220.0);
        note.set_format(format);
        note.set_channels(1);
        note.set_time(0.5);

        std::string pcm;
        jlib::sys::read(note, pcm);

        if(!check_pcm(pcm, format, 1, "datastream U8"))
            return 1;

        if(should_play(mode, format)) {
                notestream again(220.0);
            again.set_format(format);
            again.set_channels(1);
            again.set_time(0.5);

            PortAudioSink dsp;
            dsp.play(again);
        }
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }

    exit(0);
}
