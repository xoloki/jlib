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

#include <sigc++/sigc++.h>

#include <jlib/sys/sync.hh>
#include <jlib/sys/Servent.hh>

#include <jlib/media/stream.hh>
#include <jlib/media/Dsp.hh>

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

            void send_pulse(bool force);

            stream* m_stream;
            sys::sync<stream*> m_new_stream;
            sys::sync<bool> m_playing;
            sys::pipe m_beat_pipe;
            int m_beats;
            bool m_loop;

            beat_type m_beat, m_last_beat;

            Dsp m_dsp;
            int m_frags_desired;
        };
    }
}

#endif //JLIB_MEDIA_PLAYER_HH
