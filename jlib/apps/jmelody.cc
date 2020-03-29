#include <iostream>
#include <sstream>

#include <cmath>

#include <jlib/media/notestream.hh>
#include <jlib/media/Dsp.hh>

#include <jlib/sys/sys.hh>

void play(std::vector<std::string> song, int format, int channels);

int main(int argc, char** argv) {

    try {
        using namespace jlib::media;

        int format = Type::PCM_S16_LE;
        int channels = 2;
        std::string note;
        double t = 5;
        std::vector<std::string> melody;

        for(int i = 1; i < argc; i++) {
            // parse the note and play it
            std::string note = argv[i];

            // note is tone(letter)octave(number):time
            melody.push_back(note);
        }

        play(melody, format, channels);
        
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }

    exit(0);
}

void play(std::vector<std::string> song, int format, int channels) {
    jlib::media::Dsp dsp;

    for(auto s : song) {
        jlib::media::notestream note(s);

        note.set_format(format);
        note.set_channels(channels);

        dsp.play(note);
    }
}

