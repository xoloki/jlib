/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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

#include <functional>

#include <signal.h>
#include <sys/socket.h>

namespace jlib {
    namespace sys {

        class io_exception : public std::exception {
        public:
            io_exception(const std::string& msg = "") {
                m_msg = "io exception: "+msg;
            }
            virtual ~io_exception() {}
            virtual const char* what() const noexcept { return m_msg.c_str(); }
        protected:
            std::string m_msg;
        };
        
        class sys_exception : public std::exception {
        public:
            sys_exception(const std::string& msg = "") {
                m_msg = "sys exception: "+msg;
            }
            virtual ~sys_exception() {}
            virtual const char* what() const noexcept { return m_msg.c_str(); }
        protected:
            std::string m_msg;
        };

        /**
         * read a line from is into s, doing intelligent buffering
         */
        /**
         * Stop writes to this socket from killing the process.
         *
         * Writing to a socket whose peer has closed raises SIGPIPE, and the
         * default disposition of SIGPIPE is to terminate -- so the error return
         * the caller is carefully checking never arrives.  For a mail client
         * that is not an exotic case: a server dropping an idle IMAP connection
         * is routine, and the symptom is the program vanishing without a word.
         *
         * Where the platform has SO_NOSIGPIPE this sets it, which covers every
         * write on the descriptor including the ones OpenSSL makes internally.
         * Where it does not -- Linux -- this does nothing and sigpipe_guard is
         * what does the work.
         */
        void nosigpipe(int fd);

        /**
         * Blocks SIGPIPE for the calling thread, and consumes one if it comes.
         *
         * For platforms without SO_NOSIGPIPE, where the alternatives are worse:
         * MSG_NOSIGNAL is per-call and so cannot cover OpenSSL's own writes, and
         * ignoring SIGPIPE process-wide is not a library's decision to make.
         *
         * Blocking it is, since the block is per-thread and undone on the way
         * out.  A SIGPIPE raised while blocked stays pending, so the destructor
         * drains it before unblocking; otherwise it would be delivered to the
         * caller the moment the mask was restored, which is the same crash a
         * little later.
         *
         * A no-op where nosigpipe() has already dealt with it.
         */
        class sigpipe_guard {
        public:
            sigpipe_guard();
            ~sigpipe_guard();

            sigpipe_guard(const sigpipe_guard&) = delete;
            sigpipe_guard& operator=(const sigpipe_guard&) = delete;

        private:
#ifndef SO_NOSIGPIPE
            sigset_t m_old;
            bool m_blocked;   // we are the ones who blocked it
#endif
        };

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
        void thread(const std::function<void()>& slot, const std::string& s="");

        /**
         * lock a global mutex referred to by s
         */
        void lock(const std::string& s);

        /**
         * lock a global mutex referred to by s
         */
        void unlock(const std::string& s);

        /**
         * is the mutex locked?
         */
        bool locked(const std::string& s);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails
         * 
         */
        void shell(const std::string& cmd);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails.  return stdout and stderr in the passed
         * strings.
         */
        void shell(const std::string& cmd, std::string& out, std::string& err);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails.  return stdout and stderr in the passed
         * strings.
         *
         * this version of the function also allows you to pass data into 
         * the shell command using stdin.  The flag input_file tells whether
         * the parameter 'in' is a file path or a plain string.
         */
        void shell(const std::string& cmd, const std::string& in, std::string& out, std::string& err, bool in_file=true);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails.  return stdout and stderr in the passed
         * strings. The tmp files used by this function are reasonably
         * secure, in that they will be overwritten before deletion
         * to keep anyone from recovering the contents.
         *
         */
        void secure_shell(const std::string& cmd, std::string& out, std::string& err);

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
        void secure_shell(const std::string& cmd, const std::string& in, std::string& out, std::string& err, bool in_file=false);


    }
}
#endif //JLIB_SYS_SYS_HH
