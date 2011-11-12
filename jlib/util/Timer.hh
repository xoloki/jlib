/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joe Yandle <jwy@divisionbyzero.com>
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
                exception(std::string msg = "") {
                    m_msg = "jlib::util::Timer::exception: "+msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
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
