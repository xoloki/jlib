#include <iostream>
#include <sstream>

#include <cmath>

#include <jlib/media/notestream.hh>
#include <jlib/media/Dsp.hh>

#include <jlib/sys/sys.hh>

const long double 	PI = 3.14159265358979323846264338;

void play(double freq, int format, int channels, double t);
void play(std::string note, int format, int channels, double t);
void play(jlib::media::notestream& ns, int format, int channels, double t);

int main(int argc, char** argv) {

    try {
        using namespace jlib::media;

        double freq = 220;
        int format = Type::PCM_S16_LE;
        int channels = 2;
        std::string note;
        double t = 5;
        bool isnote = false;

        if(argc > 1) {
            isnote = !std::isdigit(argv[1][0]);

            std::istringstream i(argv[1]);

            if(isnote)
                i >> note;
            else
                i >> freq;
        }
        
        if(argc > 2) {
            std::istringstream i(argv[2]);
            i >> t;
        }

        if(isnote)
            play(note, format, channels, t);
        else
            play(freq, format, channels, t);
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }

    exit(0);
}

// note denotes the frequency
void play(std::string note, int format, int channels, double t) {
    jlib::media::notestream ns(note);
    play(ns, format, channels, t);
}

void play(double freq, int format, int channels, double t) {
    jlib::media::notestream ns(freq);
    play(ns, format, channels, t);
}

void play(jlib::media::notestream& ns, int format, int channels, double t) {
    jlib::media::Dsp dsp;

    ns.set_format(format);
    ns.set_channels(channels);
    //ns.set_nearest_time(t);
    ns.set_time(t);

    dsp.play(ns);
    
}
