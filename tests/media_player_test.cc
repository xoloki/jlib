#include <iostream>
#include <fstream>

#include <unistd.h>

#include <cmath>

#include <jlib/media/datastream.hh>
#include <jlib/media/notestream.hh>
#include <jlib/media/Player.hh>

#include <chrono>
#include <cstdlib>
#include <thread>
#include <jlib/media/PortAudioSink.hh>

#include <jlib/sys/sys.hh>

#include "audio_test.hh"

const long double 	PI = 3.14159265358979323846264338;



/**
 * Fail rather than hang.
 *
 * Everything below is about starting and stopping threads, so the way it goes
 * wrong is a deadlock, and a deadlocked test hangs the whole suite until
 * somebody notices.  A watchdog turns that into an ordinary failure with a
 * message saying where.
 */
static void watchdog(const char* what, int seconds) {
    std::thread([what, seconds]() {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        std::cerr << "media_player_test: deadlock in " << what
                  << " -- gave up after " << seconds << "s" << std::endl;
        std::abort();
    }).detach();
}

/**
 * Start and stop a Player without ever playing anything.
 *
 * Worth running with no device and no sound, because it covers the part most
 * likely to be wrong: Player now runs two threads, a command worker and a
 * feeder, and both touch members of Player.  Both have to be stopped before
 * those members are destroyed, and the feeder can be parked inside the sink
 * waiting for room when the time comes to stop it.
 *
 * The sink only opens a device on the first play, so all of this runs
 * anywhere -- including the container, where there is no device at all.
 */
static int lifecycle() {
    using namespace jlib::media;

    int failures = 0;

    watchdog("lifecycle", 30);

    {
        // built and destroyed, never told to do anything
        notestream note(220.0);
        Player p(&note);
    }
    std::cout << "  ok   construct and destroy\n";

    {
        // told to stop without ever having played
        notestream note(220.0);
        Player p(&note);
        p.stop();
    }
    std::cout << "  ok   stop without play\n";

    {
        // the stream swapped underneath it, which reload_signal handles on the
        // worker while the feeder is waiting on the same state
        notestream a(220.0), b(440.0);
        Player p(&a);
        p.set_stream(&b);
        p.stop();
    }
    std::cout << "  ok   stream replaced\n";

    {
        // several in a row, since a leaked or unjoined thread shows up here
        for(int i = 0; i < 8; i++) {
            notestream note(220.0);
            Player p(&note);
            p.stop();
        }
    }
    std::cout << "  ok   eight in succession\n";

    return failures;
}

int main(int argc, char** argv) {
    using namespace jlib::media;
    using namespace jlib::tests;

    if(lifecycle() != 0)
        return 1;

    // The rest drives a real device, so there is nothing to verify without
    // one.  Silent by default means skip.
    const audio_mode mode = get_audio_mode(argc, argv);

    if(mode == AUDIO_SILENT) {
        std::cerr << "playback needs a device; pass --play to exercise it" << std::endl;
        return 0;
    }

    if(!PortAudioSink::have_output_device()) {
        std::cerr << "no audio output device, skipping" << std::endl;
        return 77;
    }

    try {
        notestream note(220.0);
        // Float32, not U8.  The other media tests moved to clean formats when
        // is_clean_format() went into audio_test.hh -- 8-bit is about 48dB of
        // signal to noise and the quantization is plainly audible -- and this
        // one was missed.
        note.set_format(Type::PCM_FLOAT32);
        note.set_channels(2);
        note.set_time(1);

        watchdog("playback", 30);

        Player p(&note);
        p.play();
        for(int i=0;i<2;i++)
            sleep(1);

        // stop while the feeder may be parked inside the sink waiting for the
        // ring to drain: it has to be woken, not left there
        p.stop();
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }

    exit(0);
}
