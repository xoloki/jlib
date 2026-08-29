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

            /**
             * Reserved command ids.  A caller's own ids must be >= 0.
             *
             * WAKE exists because the loop now blocks: with nothing periodic
             * registered it sleeps in poll() until a command arrives, so
             * anything that changes whether periodic work *exists* has to
             * knock.  Reading it does nothing, which is the whole job.
             */
            static const id_type EXIT = -1;
            static const id_type WAKE = -2;

            Servent();
            virtual ~Servent();

            void map(id_type command, std::function<void()> slot);

            /**
             * Add a predicate and what to run when it holds.
             *
             * Evaluated once per pass of the loop, so registering one puts the
             * worker into polling mode -- see start().  Safe to call while the
             * worker is running: this wakes it so it notices.
             */
            void add(condition_list_type::value_type condition);

            /**
             * Knock, so a blocked worker goes round once more.
             *
             * Only needed after connecting to cycle while the worker is
             * already running, since that is the one registration this class
             * cannot see happen -- cycle is a public member and connecting to
             * it does not go through here.  add() knocks for itself, and
             * map() needs no knock: a command wakes the loop by arriving.
             * A no-op if nothing is running.
             */
            void wake();

            void exec(id_type command, int maxwait=-1);

            void run();

            /**
             * The worker loop: read a command, dispatch it, evaluate the
             * conditions, emit cycle.
             *
             * **It blocks when nothing periodic is registered, and polls when
             * something is.**  Those are two different classes wearing one
             * name: a command dispatcher has nothing to do between commands
             * and should sleep, while a caller who registered a condition or
             * connected to cycle asked for something to happen on a tick and
             * must keep getting it.  So the cadence follows the configuration
             * rather than a constant nobody chose.
             *
             * It used to poll at 1ms unconditionally, which cost about 13ms of
             * CPU per idle second whether or not anyone wanted a tick.
             *
             * The polling interval is still 1ms and still not configurable;
             * that is the next question, not this one.
             */
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
            /** True when someone has asked for work on every pass. */
            bool periodic();

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
