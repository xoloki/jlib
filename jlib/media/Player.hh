/* -*- mode: C++ c-basic-offset: 4 -*-
 * Player.hh
 * Copyright (c) 2002 Joey Yandle <jwy@divisionbyzero.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 * 
 */

#ifndef JLIB_MEDIA_PLAYER_HH
#define JLIB_MEDIA_PLAYER_HH

#include <jlib/sys/signal.hh>

#include <jlib/sys/sync.hh>
#include <jlib/sys/Servent.hh>

#include <jlib/media/stream.hh>
#include <jlib/media/PortAudioSink.hh>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <sys/poll.h>

namespace jlib {
    namespace media {
        class Player : public sys::Servent {
        public:
            typedef sys::Servent::id_type id_type;
            typedef int                   beat_type;

            static const id_type PLAY   = 0x0;
            static const id_type PAUSE  = 0x1;
            static const id_type STOP   = 0x2;
            static const id_type FFWD   = 0x3;
            static const id_type REWIND = 0x4;
            static const id_type RELOAD = 0x5;
            static const id_type NOOP   = 0xff;

            Player(stream* stream=0);
            virtual ~Player();

            void init();
            void kill();

            void play();
            void pause();
            void stop();
            void rewind();
            void ffwd();
            void reload();
            
            void set_beats(int s);
            int get_beats() const;

            sys::pipe& get_beat_pipe();
            const sys::pipe& get_beat_pipe() const;

            void set_stream(stream* s);
            stream* get_stream();
            const stream * const get_stream() const;

            void set_loop(bool s);
            bool get_loop() const;

            bool is_playing();

        protected:

            void play_signal();
            void pause_signal();
            void stop_signal();
            void rewind_signal();
            void ffwd_signal();
            void reload_signal();

            void play_slot();

            /**
             * The feeder: moves samples from the stream into the sink.
             *
             * Its own thread, because the write it makes blocks when the sink's
             * ring is full.  That used to happen on the Servent worker, which
             * is also the thread reading the command pipe, so a PAUSE or a STOP
             * waited behind the device -- #36.  Nothing on this thread reads
             * commands, so blocking here costs nothing.
             */
            void feed();

            /**
             * Recompute whether the feeder has work, and wake it.
             *
             * Call after anything that changes whether there is audio to move:
             * play, pause, stop, a new stream, end of stream.
             */
            void update_feed_state();

            void send_pulse(bool force);

            stream* m_stream;
            sys::sync<stream*> m_new_stream;
            sys::sync<bool> m_playing;
            sys::pipe m_beat_pipe;
            int m_beats;
            bool m_loop;

            beat_type m_beat, m_last_beat;

            PortAudioSink m_sink;
            int m_periods_desired;

            std::thread m_feeder;

            /** False tells the feeder to finish; see ~Player. */
            std::atomic<bool> m_feeding;

            /**
             * Whether the feeder has anything to do, as a plain atomic.
             *
             * Deliberately not read from m_playing, which is a sync<bool> with
             * a lock of its own.  The feeder holds m_feed_lock while testing
             * its predicate, and the setters hold m_playing's lock before
             * taking m_feed_lock -- reading m_playing inside the predicate
             * would take the two in opposite orders and could deadlock.  One
             * atomic, written under the same discipline as the rest, avoids it.
             */
            std::atomic<bool> m_feed_go;

            std::mutex m_feed_lock;
            std::condition_variable m_feed_wake;

            /**
             * Guards m_stream and the position within it.
             *
             * The feeder reads samples out of the stream while the command
             * worker may be seeking it, swapping it, or rewinding it -- which
             * were the same thread until the feeder existed, and so needed
             * nothing.
             *
             * Held only across the read, never across the write to the sink.
             * The write blocks until the device has room, and holding this
             * over it would make every transport command wait for the device
             * again -- the very thing the feeder exists to avoid.
             */
            std::mutex m_stream_lock;
        };
    }
}

#endif //JLIB_MEDIA_PLAYER_HH
