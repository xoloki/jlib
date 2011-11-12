/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joe Yandle <jwy@divisionbyzero.com>
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

#include <jlib/util/util.hh>
#include <jlib/util/Timer.hh>

#include <algorithm>

namespace jlib {
    namespace util {
        
        Timer::Timer() {
            init();
        }

        Timer::Timer(const Timer& r) {
            copy(r);
        }

        Timer::~Timer() {
            
        }
        
        Timer& Timer::operator=(const Timer& r) {
            copy(r);
            return *this;
        }
        
        void Timer::start() {
            gettimeofday(&m_start,0);
            m_running = true;
        }

        void Timer::stop() {
            gettimeofday(&m_stop,0);
            m_total = get_diff();
            m_running = false;
        }
        
        void Timer::clear() {
            init();
        }

        std::string Timer::operator()() {
            return string_value(get_time());
        }

        double Timer::get_time() {
            if(m_running) {
                struct timeval tmp;
                gettimeofday(&tmp,0);
                return get_diff(&tmp);
            }
            else {
                return m_total;
            }
        }

        double Timer::get_diff(struct timeval* t) {
            if(t == 0) {
                t = &m_stop;
            }
                
            long sec_diff, usec_diff;

            sec_diff = (t->tv_sec - m_start.tv_sec);
            usec_diff = (t->tv_usec - m_start.tv_usec);

            if(usec_diff < 0) {
                usec_diff = (-1)*(usec_diff);
                sec_diff  = sec_diff-1;
            }
            
            return ( sec_diff + (usec_diff/1000000.0) );
        }
        
        void Timer::init() {
            m_start.tv_sec  = 0;
            m_start.tv_usec = 0;
            
            m_stop.tv_sec  = 0;
            m_stop.tv_usec = 0;

            m_total = 0;
        }

        void Timer::copy(const Timer& r) {
            m_start.tv_sec  = r.m_start.tv_sec;
            m_start.tv_usec = r.m_start.tv_usec;

            m_stop.tv_sec  = r.m_stop.tv_sec;
            m_stop.tv_usec = r.m_stop.tv_usec;

            m_total = r.m_total;
            m_running = r.m_running;
        }
        
        bool Timer::get_running() {
            return m_running;
        }

    }
}
