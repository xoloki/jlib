/* -*- mode: C++ c-basic-offset: 4 -*-
 * Directory.h
 * Copyright (c) 2000 Joe Yandle <jwy@divisionbyzero.com>
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
                exception(std::string msg = "") {
                    m_msg = "jlib::sys::Directory exception: "+msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            Directory(std::string p_path = "/");
            Directory(const Directory& p_copy);
            virtual ~Directory() {}
            
            Directory& operator=(const Directory& p_copy);
            void copy(const Directory& p_copy);
            
            std::string get_path() const;
            std::string get_name() const;
            Directory sub(std::string file) const;
            
            std::vector<std::string> list_files(bool p_full_path = false) const;
            std::vector<std::string> list_dirs(bool p_full_path = false) const;
            std::vector<Directory> list_subdirs() const;
            
            bool is(std::string p_file, file_type p_type) const;
            
            std::vector<std::string> list(file_type p_type = ALL, bool p_full_path=false, bool p_show_dots = false) const;
            
        protected:
            std::string m_path;
            std::string m_name;
        };
    }
}

#endif //JLIB_SYS_DIRECTORY_HH
