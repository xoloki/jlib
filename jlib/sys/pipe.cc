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
            // This was a function-local static, shared by every thread calling
            // poll() -- and Servent calls it from both its worker and from
            // exec() on the caller's thread.
            struct pollfd fds;
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
