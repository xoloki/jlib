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
