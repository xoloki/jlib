/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2002 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_SYS_PIPE_HH
#define JLIB_SYS_PIPE_HH

#include <sys/poll.h>

#include <exception>
#include <string>
#include <sstream>
#include <cstring>


#include <errno.h>
#include <unistd.h>

namespace jlib {
    namespace sys {

        class pipe {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = "jlib::sys::pipe exception"+
                        (msg != "" ? (": "+msg):"");
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
                
                [[noreturn]] static void throw_errno(const std::string& msg) {
                    std::ostringstream o;
                    o << ((msg!="")?(msg+": "):"") << strerror(errno);
                    throw exception(o.str());
                }

            protected:
                std::string m_msg;
            };

            class would_block : public std::exception {
            public:
                virtual const char* what() const noexcept { 
                    return "jlib::sys::pipe: i/o would block";
                }
            };


            typedef int event_type;

            static const event_type IN = POLLIN;
            static const event_type PRI = POLLPRI;
            static const event_type OUT = POLLOUT;
            static const event_type ERR = POLLERR;
            static const event_type HUP = POLLHUP;
            static const event_type NVAL = POLLNVAL;

            pipe(bool block_read=true, bool block_write=true);
            ~pipe();

            bool poll(event_type event_mask=IN, int wait=1);

            template<typename T> 
            T read();

            int read_int();
            
            template<typename T>
            void write(const T& t);

            void write_int(int t);

            int get_reader() const;
            int get_writer() const;

            // Deleted, and the leak below is why it matters: with a raw int*
            // member and no copy control, copying one gave two objects owning
            // the same array -- a double delete[], and once the destructor
            // learned to close, a double close of two live descriptors.  A
            // pipe is a pair of open files; it is not a value.
            pipe(const pipe&) = delete;
            pipe& operator=(const pipe&) = delete;

        private:
            // An array of two, not a new int[2].  Nothing was ever gained by
            // the allocation and the destructor deleted it without closing
            // either descriptor -- so every Servent and every ASServent, both
            // of which hold one for their lifetime, leaked two.
            int m_pipe[2] = { -1, -1 };
            bool m_block_read;
            bool m_block_write;
        };

        template<class T> 
        inline
        T pipe::read() {
            T r;
            char* p = reinterpret_cast<char*>(&r);
            size_t n = ::read(m_pipe[0], p, sizeof(T));
            if(n == -1)
                if(errno == EAGAIN) 
                    throw would_block();
                else
                    exception::throw_errno("read(): error reading from pipe");
            else if(n != sizeof(T)) 
                throw exception("read(): n != sizeof(T)");
            
            return r;
        }

        template<>
        inline 
        void pipe::write<std::string>(const std::string& s) {
            int n = ::write(m_pipe[1], reinterpret_cast<const void*>(s.data()), s.size());
            if(n == -1)
                if(errno == EAGAIN) 
                    throw would_block();
                else
                    exception::throw_errno("write(): error writing to pipe");
            else if(n != s.size())
                throw exception("write(): n != s.size(): failed to write all data to pipe");
        }
            
        template<class T> 
        inline
        void pipe::write(const T& t) {
            int n = ::write(m_pipe[1],reinterpret_cast<const void*>(&t),sizeof(T));
            if(n == -1)
                if(errno == EAGAIN) 
                    throw would_block();
                else
                    exception::throw_errno("write(): error writing to pipe");
            else if(n != sizeof(T))
                throw exception("write(): n != sizeof(T): failed to write all data to pipe");
        }
            


    }
}

#endif //JLIB_SYS_PIPE_HH
