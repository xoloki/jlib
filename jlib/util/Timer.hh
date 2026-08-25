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

#ifndef JLIB_UTIL_TIMER_HH
#define JLIB_UTIL_TIMER_HH

#include <exception>
#include <string>

#include <sys/time.h>

namespace jlib {
    namespace util {

        class Timer {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = "jlib::util::Timer::exception: "+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            Timer();
            Timer(const Timer& r);
            ~Timer();

            Timer& operator=(const Timer& r);

            void start();
            void stop();
            void clear();

            std::string operator()();
            double get_time();

            bool get_running();
        protected:
            double get_diff(struct timeval* t=0);
            
            void init();
            void copy(const Timer& r);

            double m_total;
            struct timeval m_start;
            struct timeval m_stop;

            bool m_running;
        };
        
    }
}

#endif //JLIB_UTIL_TIMER_HH
