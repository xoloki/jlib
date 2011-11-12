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

#ifndef JLIB_UTIL_REGEX_HH
#define JLIB_UTIL_REGEX_HH

#include <iostream>
#include <exception>
#include <string>
#include <regex.h>

namespace jlib {
    namespace util {
        class Regex {
        public:
            class exception : public std::exception {
            public:
                exception(std::string p_msg = "") {
                    m_msg = "jlib::util::Regex::exception: "+p_msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            class Match {
            public:
                Match();
                Match(regmatch_t* info, unsigned int size, char* text);
                Match(const Match& m);
                ~Match();

                Match& operator=(const Match& m);
                std::string operator[](unsigned int i);

                unsigned int size() { return m_size; }
                
                operator bool();
            protected:
                void copy(const Match& m);
                void destroy();

                regmatch_t* m_info;
                unsigned int m_size;
                char* m_text;
            };

            Regex(std::string pattern="", int flags=REG_EXTENDED);
            Regex(const Regex& r);
            ~Regex();

            Match match(std::string str);
            Match operator()(std::string str);

            Regex& operator=(const Regex& r);

            std::string operator[](unsigned int i);

        protected:
            void init(std::string pattern, int flags);
            void copy(const Regex& r);

            regex_t m_regex;
            std::string m_pattern;
            unsigned int m_size;
            int m_flags;

            Match m_last;
        };
        
    }
}

#endif //JLIB_UTIL_REGEX_HH
