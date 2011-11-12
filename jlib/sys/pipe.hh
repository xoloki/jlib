/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2002 Joe Yandle <jwy@divisionbyzero.com>
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

#ifndef JLIB_SYS_PIPE_HH
#define JLIB_SYS_PIPE_HH

#include <sys/poll.h>

#include <exception>
#include <string>
#include <sstream>
#include <cstring>

#include <sigc++/sigc++.h>

#include <errno.h>
#include <unistd.h>

namespace jlib {
    namespace sys {

        class pipe {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = "jlib::sys::pipe exception"+
                        (msg != "" ? (": "+msg):"");
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
                
                static void throw_errno(std::string msg) {
                    std::ostringstream o;
                    o << ((msg!="")?(msg+": "):"") << strerror(errno);
                    throw exception(o.str());
                }

            protected:
                std::string m_msg;
            };

            class would_block : public std::exception {
            public:
                virtual const char* what() const throw() { 
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

        private:
            int* m_pipe;
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
