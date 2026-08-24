/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2002 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_SYS_SERVENT_HH
#define JLIB_SYS_SERVENT_HH

#include <jlib/sys/pipe.hh>
#include <jlib/sys/object.hh>
#include <jlib/sys/signal.hh>
#include <jlib/sys/sync.hh>

#include <exception>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <cstring>

#include <errno.h>

namespace jlib {
    namespace sys {

        class Servent : public Object {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = "jlib::sys::Servent exception"+
                        (msg != "" ? (": "+msg):"");
                }
                virtual ~exception() noexcept {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
                
                static void throw_errno(const std::string& msg) {
                    std::ostringstream o;
                    o << ((msg!="")?(msg+": "):"") << strerror(errno);
                    throw exception(o.str());
                }

            protected:
                std::string m_msg;
            };

            typedef int                                        id_type;
            typedef std::map< id_type, std::function<void()> > command_map_type;

            typedef std::list<std::pair<std::function<bool()>, std::function<void()> > >
                                                               condition_list_type;

            static const id_type EXIT = -1;

            Servent();
            virtual ~Servent();

            void map(id_type command, std::function<void()> slot);
            void add(condition_list_type::value_type condition);

            void exec(id_type command, int maxwait=-1);

            void run();

            void start();

            /**
             * Stop the worker and wait for it to finish.
             *
             * A derived class must call this from its own destructor.  ~Servent
             * calls it too, but by then the derived part of the object is
             * already gone, so anything the worker touches that a subclass owns
             * must be shut down earlier than that.
             */
            void stop();

            signal<void()> cycle;

        protected:
            pipe m_pipe;
            command_map_type m_commands;
            condition_list_type m_conditions;
            std::thread m_worker;
            std::mutex m_lock;
            sync<bool> m_bunny;
        };

    }
}

#endif //JLIB_SYS_SERVENT_HH
