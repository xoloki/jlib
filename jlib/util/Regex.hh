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
                exception(const std::string& p_msg = "") {
                    m_msg = "jlib::util::Regex::exception: "+p_msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
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

            Regex(const std::string& pattern="", int flags=REG_EXTENDED);
            Regex(const Regex& r);
            ~Regex();

            Match match(const std::string& str);
            Match operator()(const std::string& str);

            Regex& operator=(const Regex& r);

            std::string operator[](unsigned int i);

        protected:
            void init(const std::string& pattern, int flags);
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
