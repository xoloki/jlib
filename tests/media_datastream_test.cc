#include <iostream>
#include <fstream>

#include <unistd.h>

#include <cmath>

#include <jlib/media/datastream.hh>
#include <jlib/media/notestream.hh>
#include <jlib/media/Dsp.hh>

#include <jlib/sys/sys.hh>

const long double 	PI = 3.14159265358979323846264338;



int main(int argc, char** argv) {
    using namespace jlib::media;

    try {
        Dsp dsp;

        notestream note(220.0);
        note.set_format(Type::PCM_U8);
        note.set_channels(1);
        note.set_time(0.5);

        dsp.play(note);
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }

    exit(0);
}
