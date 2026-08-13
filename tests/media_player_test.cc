#include <iostream>
#include <fstream>

#include <unistd.h>

#include <cmath>

#include <jlib/media/datastream.hh>
#include <jlib/media/notestream.hh>
#include <jlib/media/Player.hh>
#include <jlib/media/PortAudioSink.hh>

#include <jlib/sys/sys.hh>

#include "audio_test.hh"

const long double 	PI = 3.14159265358979323846264338;



int main(int argc, char** argv) {
    using namespace jlib::media;
    using namespace jlib::tests;

    // Player drives a real device, so unlike the other media tests there is
    // nothing to verify without one.  Silent by default means skip.
    const audio_mode mode = get_audio_mode(argc, argv);

    if(mode == AUDIO_SILENT) {
        std::cerr << "Player needs a device; pass --play to exercise it" << std::endl;
        return 77;
    }

    if(!PortAudioSink::have_output_device()) {
        std::cerr << "no audio output device, skipping" << std::endl;
        return 77;
    }

    try {
        notestream note(220.0);
        note.set_format(Type::PCM_U8);
        note.set_channels(2);
        note.set_time(1);

        Player p(&note);
        p.play();
        for(int i=0;i<2;i++)
            sleep(1);
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }

    exit(0);
}
