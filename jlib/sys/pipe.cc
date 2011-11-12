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

#include <jlib/sys/pipe.hh>

#include <iostream>

#include <cstdlib>

#include <unistd.h>
#include <fcntl.h>

namespace jlib {
    namespace sys {

        pipe::pipe(bool block_read, bool block_write)
            : m_block_read(block_read),
              m_block_write(block_write)
        {
            m_pipe = new int[2];
            if(::pipe(m_pipe) == -1) {
                exception::throw_errno("unable to create pipe");
            }
            if(!block_read) {
                int flags = fcntl(m_pipe[0], F_GETFL);
                if(flags == -1) {
                    exception::throw_errno("unable to read flags");
                }
                int r = fcntl(m_pipe[0], F_SETFL, (flags | O_NONBLOCK));
                if(r == -1) {
                    exception::throw_errno("unable to set O_NONBLOCK flag");
                }
            }
            if(!block_write) {
                int flags = fcntl(m_pipe[1], F_GETFL);
                if(flags == -1) {
                    exception::throw_errno("unable to read flags");
                }
                int r = fcntl(m_pipe[1], F_SETFL, (flags | O_NONBLOCK));
                if(r == -1) {
                    exception::throw_errno("unable to set O_NONBLOCK flag");
                }
            }
        }

        pipe::~pipe() {
            delete [] m_pipe;
        }
        
        int pipe::read_int() {
            return read<int>();
        }
        
        void pipe::write_int(int t) {
            write<int>(t);
        }
        
        bool pipe::poll(event_type event_mask, int wait) {
            static struct pollfd fds;
            if(event_mask == IN)
                fds.fd = m_pipe[0];
            else if(event_mask == OUT)
                fds.fd = m_pipe[1];
            else
                throw exception("did you really want input and output from the pipe?");

            fds.events = event_mask;
            int e = ::poll(&fds, 1, wait);

            if(std::getenv("JLIB_SYS_PIPE_DEBUG"))
                std::cerr << "jlib::sys::pipe::poll(): e = " << e << std::endl;

            if(e == -1) {
                if(errno == EINTR) 
                    return false;
                else
                    exception::throw_errno("poll(): error in poll()");
            }
            else if(e == 1) {
                if(std::getenv("JLIB_SYS_PIPE_DEBUG"))
                    std::cerr << "jlib::sys::pipe::poll(): fds.revents = " 
                              << std::hex << fds.revents << std::endl;
                if(fds.revents & event_mask)
                    return true;
                else if(fds.revents & ERR)
                    throw exception("poll(): revents & ERR");
                else if(fds.revents & HUP)
                    throw exception("poll(): revents & HUP");
                else if(fds.revents & NVAL)
                    throw exception("poll(): revents & NVAL");
                else
                    return false;
            }
            else if(e == 0) {
                return false;
            }
            else {
                throw exception("too many fds returned from poll()");
            }

        }

        int pipe::get_reader() const {
            return m_pipe[0];
        }
        
        int pipe::get_writer() const {
            return m_pipe[1];
        }
        
    }
}
