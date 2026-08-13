/* -*- mode: C++ c-basic-offset: 4 -*-
 * Player.cc
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

#include <jlib/media/Player.hh>

#include <jlib/sys/sys.hh>

#include <jlib/util/util.hh>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <fcntl.h>

#include <cmath>

#include <errno.h>
#include <unistd.h>

#include <sstream>

/*
 * as long as PLAYER_PLAYING_POLL_WAIT = 0, fragsize can be as small as 512,
 * otherwise you should keep it at 2048 (emu10k1)
 */

const int PLAYER_FRAGS_BUFFERED = 3;
const int PLAYER_FRAGSIZE = 2048;
const int PLAYER_PLAYING_POLL_WAIT = 1;
const int PLAYER_NOT_PLAYING_POLL_WAIT = 1;

const jlib::media::basic_streambuf<char>::off_type PLAYER_INCREMENT = 512;

namespace jlib {
    namespace media {
            
        
        Player::Player(stream* s) 
            : m_stream(s),
              m_new_stream(s),
              m_playing(false),
              m_beats(-1),
              m_loop(false),
              m_beat(0),
              m_last_beat(0),
              m_periods_desired(2)
        {
            init();
            if(s)
                set_stream(s);
        }

        
        Player::~Player() {
            // The worker touches m_sink and m_stream, both members of Player.
            // Leaving this empty meant those were destroyed -- closing the
            // audio device -- while the worker was potentially still inside
            // play_slot(), because ~Servent only runs after ~Player returns.
            stop();
        }
            
        
        void Player::set_stream(stream* s) {
            m_new_stream = s;
            reload();
        }

        const stream* const Player::get_stream() const {
            return m_stream;
        }

        stream* Player::get_stream() {
            return m_stream;
        }


        void Player::init() {
            map(PLAY, [this](auto&&... a) { return this->play_signal(a...); });
            map(PAUSE, [this](auto&&... a) { return this->pause_signal(a...); });
            map(STOP, [this](auto&&... a) { return this->stop_signal(a...); });
            map(REWIND, [this](auto&&... a) { return this->rewind_signal(a...); });
            map(FFWD, [this](auto&&... a) { return this->ffwd_signal(a...); });
            map(RELOAD, [this](auto&&... a) { return this->reload_signal(a...); });

            // Built as the pair's own type rather than with std::make_pair,
            // which would deduce the two lambda types and leave no conversion
            // to pair<function<bool()>, function<void()>>.
            add(condition_list_type::value_type([this]() { return is_playing(); },
                                                [this]() { play_slot(); }));

            run();
        }

        
		// This used to throw Glib::Thread::Exit() on the *calling* thread,
		// which never touched the worker at all.  Servent::stop() asks the
		// worker to exit and joins it.
		void Player::kill() { stop(); }

        void Player::play() { exec(Player::PLAY); }
        void Player::pause() { exec(Player::PAUSE); }
        void Player::stop() { exec(Player::STOP); }
        void Player::rewind() { exec(Player::REWIND); }
        void Player::ffwd() { exec(Player::FFWD); }
        void Player::reload() { exec(Player::RELOAD); }


        void Player::play_signal() { 
            if(getenv("JLIB_MEDIA_PLAYER_DEBUG")) 
                std::cerr << "received PLAY command" << std::endl;
            m_playing = !m_playing;
            if(!m_playing)
                m_sink.reset();
        }

        void Player::pause_signal() { 
            if(getenv("JLIB_MEDIA_PLAYER_DEBUG")) 
                std::cerr << "\treceived PAUSE command" << std::endl;
            m_playing = false;
            m_sink.reset();
        }

        void Player::stop_signal() { 
            if(getenv("JLIB_MEDIA_PLAYER_DEBUG")) 
                std::cerr << "\treceived STOP command" << std::endl;
            m_playing = false;
            if(m_stream) {
                m_sink.reset();
                m_stream->rewind();
            
                send_pulse(true);
            }

        }

        void Player::rewind_signal() { 
            if(getenv("JLIB_MEDIA_PLAYER_DEBUG")) 
                std::cerr << "\treceived REWIND command" << std::endl;

            if(m_stream) {
                m_sink.reset();
                m_stream->clear();
                jlib::media::basic_streambuf<char>::off_type increment = static_cast<jlib::media::basic_streambuf<char>::off_type>(m_stream->get_length() / get_beats());

                increment -= (increment % (m_stream->get_bits_per_sample()/8));

                if(m_stream->tellg() >= increment)
                    m_stream->seekg(-increment,std::ios_base::cur);
                else
                    m_stream->rewind();
                
                send_pulse(true);
            }

        }

        void Player::ffwd_signal() { 
            if(getenv("JLIB_MEDIA_PLAYER_DEBUG")) 
                std::cerr << "\treceived FFWD command" << std::endl;
            
            if(m_stream) {
                m_sink.reset();
                m_stream->clear();
                
                basic_streambuf<char>::off_type increment = static_cast<basic_streambuf<char>::off_type>(m_stream->get_length() / get_beats());
                
                increment -= (increment % (m_stream->get_bits_per_sample()/8));

                if(m_stream->tellg()+increment  < m_stream->get_length())
                    m_stream->seekg(increment,std::ios_base::cur);
                else
                    m_stream->rewind();
                
                send_pulse(true);
            }

        }

        void Player::reload_signal() { 
            if(getenv("JLIB_MEDIA_PLAYER_DEBUG")) 
                std::cerr << "\treceived RELOAD command" << std::endl;

            basic_streambuf<char>::pos_type p = 0;
            if(m_stream)
                p = m_stream->tellg();
            m_stream = m_new_stream;
            if(m_stream && p < m_stream->get_length())
                m_stream->seekg(p);

            m_sink.config(*m_stream);
            m_sink.reset();
        }

        void Player::send_pulse(bool force) {
            if(m_stream != 0 && static_cast<int>(m_stream->get_length()) != -1) {
                m_beat = (beat_type)(((double)m_stream->tellg() / (double)static_cast<int>(m_stream->get_length())) * get_beats());
                if(force || m_last_beat != m_beat) {
                    m_last_beat = m_beat;
                    if(getenv("JLIB_MEDIA_PLAYER_DEBUG"))
                        std::cerr << "\tsending beat " << (int)m_beat << std::endl;
                    m_beat_pipe.write<beat_type>(m_beat);
                }
            }
        }


        void Player::play_slot() {
            if(getenv("JLIB_MEDIA_PLAYER_DEBUG")) {
                std::cerr << "enter jlib::media::Player::play_slot()" << std::endl;
            }

            if(!m_stream->eof()) {
                send_pulse(false);

                // Top the device up to m_periods_desired periods queued.  This
                // is counted in frames now rather than OSS fragments, which
                // were a byte count unrelated to frame size.
                const int want = m_periods_desired * m_sink.get_period();
                const int have = m_sink.queued();

                if(have < want) {
                    const int periods = (want - have + m_sink.get_period() - 1) / m_sink.get_period();
                    m_sink.play_frag(*m_stream, periods);
                }

                if(getenv("JLIB_MEDIA_PLAYER_DEBUG")) {
                    std::cerr << "\t" << have << "/" << want << " frames queued" << std::endl;
                }
            }
            
            if(m_stream->eof()) {
                if(m_loop)
                    m_stream->rewind();
                else
                    m_playing = false;
            }
            if(getenv("JLIB_MEDIA_PLAYER_DEBUG")) {
                std::cerr << "leave jlib::media::Player::play_slot()" << std::endl;
            }
        }

        void Player::set_beats(int s) { m_beats = s; }
        int Player::get_beats() const { return m_beats; }
        
        sys::pipe& Player::get_beat_pipe() { return m_beat_pipe; }
        const sys::pipe& Player::get_beat_pipe() const { return m_beat_pipe; }

        void Player::set_loop(bool s) { m_loop = s; }
        bool Player::get_loop() const { return m_loop; }

        bool Player::is_playing() {
            return (m_playing && m_stream);
        }

    }
}
