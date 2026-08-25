/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_SYS_TFSTREAM_HH
#define JLIB_SYS_TFSTREAM_HH

#include <fstream>
#include <exception>
#include <string>

namespace jlib {
    namespace sys {
        
        class tfstream : public std::fstream {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = "jlib::sys::tfstream exception: "+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            tfstream();//
            virtual ~tfstream();//
            std::string get_path() { return (m_file); }
            long size();

        protected:
            std::string m_dir;
            std::string m_file;
        };

        class stfstream : public tfstream {
        public:
            stfstream() : tfstream() {}
            virtual ~stfstream();
        };
        
    }
}

#endif
