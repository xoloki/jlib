/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
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

#ifndef JLIB_SYS_SYS_HH
#define JLIB_SYS_SYS_HH

#include <exception>
#include <iostream>
#include <string>

#include <sigc++/slot.h>

namespace jlib {
    namespace sys {

        class io_exception : public std::exception {
        public:
            io_exception(std::string msg = "") {
                m_msg = "io exception: "+msg;
            }
            virtual ~io_exception() throw() {}
            virtual const char* what() const throw() { return m_msg.c_str(); }
        protected:
            std::string m_msg;
        };
        
        class sys_exception : public std::exception {
        public:
            sys_exception(std::string msg = "") {
                m_msg = "sys exception: "+msg;
            }
            virtual ~sys_exception() throw() {}
            virtual const char* what() const throw() { return m_msg.c_str(); }
        protected:
            std::string m_msg;
        };

        /**
         * read a line from is into s, doing intelligent buffering
         */
        void getline(std::istream& is, std::string& s);

        /**
         * read a std::string n bytes long from is into s, doing intelligent buffering
         * if n is -1, read to the end of the stream
         */
        void getstring(std::istream& is, std::string& s, int n=-1);
        void read(std::istream& is, std::string& s, int n=-1);
        void read(std::istream& is, char* c, int n);


        /**
         * call the method passed in s in a new thread, then
         * kill the thread
         *
         * if s isn't "", then lock the global mutex for that key
         * before and unlock after
         *
         */
        void thread(const sigc::slot0<void>& slot, std::string s="");

        /**
         * lock a global mutex referred to by s
         */
        void lock(std::string s);

        /**
         * lock a global mutex referred to by s
         */
        void unlock(std::string s);

        /**
         * is the mutex locked?
         */
        bool locked(std::string s);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails
         * 
         */
        void shell(std::string cmd);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails.  return stdout and stderr in the passed
         * strings.
         */
        void shell(std::string cmd, std::string& out, std::string& err);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails.  return stdout and stderr in the passed
         * strings.
         *
         * this version of the function also allows you to pass data into 
         * the shell command using stdin.  The flag input_file tells whether
         * the parameter 'in' is a file path or a plain string.
         */
        void shell(std::string cmd, std::string in, std::string& out, std::string& err, bool in_file=true);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails.  return stdout and stderr in the passed
         * strings. The tmp files used by this function are reasonably
         * secure, in that they will be overwritten before deletion
         * to keep anyone from recovering the contents.
         *
         */
        void secure_shell(std::string cmd, std::string& out, std::string& err);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails.  return stdout and stderr in the passed
         * strings.  The tmp files used by this function are reasonably
         * secure, in that they will be overwritten before deletion
         * to keep anyone from recovering the contents.
         *
         * this version of the function also allows you to pass data into 
         * the shell command using stdin.  The flag input_file tells whether
         * the parameter 'in' is a file path or a plain string.
         */
        void secure_shell(std::string cmd, std::string in, std::string& out, std::string& err, bool in_file=false);


    }
}
#endif //JLIB_SYS_SYS_HH
