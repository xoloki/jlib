#include <iostream>
#include <fstream>

#include <unistd.h>

#include <cmath>

#include <jlib/media/datastream.hh>
#include <jlib/media/notestream.hh>
#include <jlib/media/Player.hh>

#include <jlib/sys/sys.hh>

const long double 	PI = 3.14159265358979323846264338;



int main(int argc, char** argv) {
    using namespace jlib::media;

    try {
        notestream note(220.0);
        note.set_format(AFMT_U8);
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
