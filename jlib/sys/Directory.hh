/* -*- mode: C++ c-basic-offset: 4 -*-
 * Directory.h
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

#ifndef JLIB_SYS_DIRECTORY_HH
#define JLIB_SYS_DIRECTORY_HH

#include <string>
#include <vector>

namespace jlib {
    namespace sys {
        enum file_type {REGULAR, SYMLINK, DIRECTORY, CHAR_DEV, BLOCK_DEV, FIFO, SOCKET, ALL};
        
        class Directory {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = "jlib::sys::Directory exception: "+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            Directory(const std::string& p_path = "/");
            Directory(const Directory& p_copy);
            virtual ~Directory() {}
            
            Directory& operator=(const Directory& p_copy);
            void copy(const Directory& p_copy);
            
            std::string get_path() const;
            std::string get_name() const;
            Directory sub(const std::string& file) const;
            
            std::vector<std::string> list_files(bool p_full_path = false) const;
            std::vector<std::string> list_dirs(bool p_full_path = false) const;
            std::vector<Directory> list_subdirs() const;
            
            bool is(const std::string& p_file, file_type p_type) const;
            
            std::vector<std::string> list(file_type p_type = ALL, bool p_full_path=false, bool p_show_dots = false) const;
            
        protected:
            std::string m_path;
            std::string m_name;
        };
    }
}

#endif //JLIB_SYS_DIRECTORY_HH
