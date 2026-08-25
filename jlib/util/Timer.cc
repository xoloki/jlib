/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
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
