#include <iostream>
#include <fstream>

#include <unistd.h>

#include <cmath>

#include <jlib/media/datastream.hh>
#include <jlib/media/WavFile.hh>
#include <jlib/media/Dsp.hh>

#include <jlib/sys/sys.hh>

int main(int argc, char** argv) {
    using namespace jlib::media;

    try {
        WavFile wav("/usr/share/sounds/card_shuffle.wav");
        wav.load_data_chunks();
        datastream data(wav.get_pcm());
        data.set<WavFile>(wav);

        Dsp dsp;
        dsp.play(data);
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }

    exit(0);
}
