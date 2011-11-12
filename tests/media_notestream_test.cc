#include <iostream>

#include <unistd.h>

#include <cmath>

#include <jlib/media/notestream.hh>
#include <jlib/media/Dsp.hh>

#include <jlib/sys/sys.hh>

const long double 	PI = 3.14159265358979323846264338;

void play(double freq, int format, int channels, std::string tag, bool out=true);

int main(int argc, char** argv) {

    try {
        using namespace jlib::media;
        
        play(110, Type::PCM_U8, 1, "Type::PCM_U8");
        play(220, Type::PCM_U8, 2, "Type::PCM_U8");
//         play(55, Type::PCM_S8, 1, "Type::PCM_S8");
//         play(55, Type::PCM_S8, 2, "Type::PCM_S8");
        play(440, Type::PCM_S16_LE, 1, "Type::PCM_S16_LE");
        play(880, Type::PCM_S16_LE, 2, "Type::PCM_S16_LE");
        //play(110, Type::PCM_U16_LE, 1, "Type::PCM_U16_LE");
        //play(110, Type::PCM_U16_LE, 2, "Type::PCM_U16_LE");
//         play(110, Type::PCM_S16_BE, 1, "Type::PCM_S16_BE");
//         play(110, Type::PCM_S16_BE, 2, "Type::PCM_S16_BE");
//         play(110, Type::PCM_U16_BE, 1, "Type::PCM_U16_BE");
//         play(110, Type::PCM_U16_BE, 2, "Type::PCM_U16_BE");

        /*
        std::cout << "\"mary had a little lamb\" in c major:" << std::endl;
        double c = jlib::media::basic_notebuf<char>::get_freq(4,220);
        double b = jlib::media::basic_notebuf<char>::get_freq(2,220);
        double a = jlib::media::basic_notebuf<char>::get_freq(0,220);
        play(c, Type::PCM_S16_LE, 2, "Type::PCM_S16_LE", false);
        play(b, Type::PCM_S16_LE, 2, "Type::PCM_S16_LE", false);
        play(a, Type::PCM_S16_LE, 2, "Type::PCM_S16_LE", false);
        play(b, Type::PCM_S16_LE, 2, "Type::PCM_S16_LE", false);
        play(c, Type::PCM_S16_LE, 2, "Type::PCM_S16_LE", false);
        play(c, Type::PCM_S16_LE, 2, "Type::PCM_S16_LE", false);
        play(c, Type::PCM_S16_LE, 2, "Type::PCM_S16_LE", false);
        */
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }

    exit(0);
}

void play(double freq, int format, int channels, std::string tag, bool out) {
    jlib::media::Dsp dsp;

    jlib::media::notestream note(freq);
    note.set_format(format);
    note.set_channels(channels);
    note.set_time(0.25);

    dsp.play(note);

    if(out)
        std::cout << tag << ": " << (channels > 1 ? "stereo" : "mono") << std::endl;

}
