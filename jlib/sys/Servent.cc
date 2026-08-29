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

#include <jlib/sys/Servent.hh>
#include <jlib/sys/auto.hh>

#include <iostream>

#include <cstdlib>

#include <sys/poll.h>
#include <unistd.h>

namespace jlib {
    namespace sys {

        Servent::Servent()
            : m_bunny(true)
        {
        }

        Servent::~Servent() {
            // Backstop only.  By the time this runs the derived part of the
            // object is already destroyed, so subclasses owning anything the
            // worker touches must call stop() from their own destructor.
            stop();
        }

        void Servent::stop() {
            if(!m_worker.joinable())
                return;

            try {
                exec(Servent::EXIT);
            }
            catch(std::exception& e) {
                // The command pipe may already be unwritable; fall back to
                // asking the loop to exit directly.
                std::cerr << "jlib::sys::Servent::stop(): " << e.what() << std::endl;
                m_bunny = false;
            }

            m_worker.join();
        }

        void Servent::map(id_type command, std::function<void()> slot) {
            std::lock_guard<std::mutex> lock(m_lock);

            command_map_type::iterator i = m_commands.find(command);
            if(i == m_commands.end()) {
                m_commands.insert(std::make_pair(command, slot));
            }
            else {
                std::ostringstream o;
                o << "error mapping command " << command << ": command already exists";
                throw exception(o.str());
            }
        }

        void Servent::add(condition_list_type::value_type condition) {
            {
                std::lock_guard<std::mutex> lock(m_lock);
                m_conditions.push_back(condition);
            }

            // Outside the lock, and only after the list has grown: the worker
            // may be asleep in poll() *because* there were no conditions, and
            // it re-reads periodic() when it wakes.  Knocking first would race
            // with the push and knocking under the lock would deadlock a
            // worker that woke and reached for it.
            wake();
        }

        void Servent::wake() {
            if(!m_worker.joinable())
                return;

            try {
                exec(WAKE);
            }
            catch(std::exception& e) {
                // Nothing to escalate to.  A knock that does not land means
                // the loop keeps sleeping until a real command arrives, which
                // is the behaviour this call was trying to improve on, not a
                // failure of anything already working.
                std::cerr << "jlib::sys::Servent::wake(): " << e.what() << std::endl;
            }
        }

        bool Servent::periodic() {
            std::lock_guard<std::mutex> lock(m_lock);

            // cycle is a public member, so connecting to it neither takes this
            // lock nor goes through any method here -- which is why wake()
            // exists and why connecting after run() needs one.  Reading
            // empty() while another thread connects is the same race emit()
            // has always had, and is not made worse here.
            return !m_conditions.empty() || !cycle.empty();
        }


        void Servent::exec(id_type command, int maxwait) {
            if(m_pipe.poll(pipe::OUT,maxwait))
                m_pipe.write<id_type>(command);
            else
                throw exception("exec: timeout waiting to write to command pipe");
        }
        
        void Servent::run() {
            if(m_worker.joinable())
                throw exception("run(): worker is already running");

            m_worker = std::thread([this]() { start(); });
        }
        
        void Servent::start() {
            while(m_bunny) {
                try {
                    // -1 blocks until a command arrives; 1 keeps the old
                    // cadence for a caller who registered periodic work.
                    // Re-read every pass, because add() can change the answer
                    // while this is running -- and knocks so that it does.
                    const int wait = periodic() ? 1 : -1;

                    if(m_pipe.poll(pipe::IN, wait)) {
                        if(std::getenv("JLIB_SYS_SERVENT_DEBUG"))
                            std::cerr << "jlib::sys::Servent::start(): m_pipe.poll(): true" << std::endl;
                        id_type command = m_pipe.read<id_type>();

                        if(command == Servent::EXIT) {
                            m_bunny = false;
                            break;
                        }

                        if(std::getenv("JLIB_SYS_SERVENT_DEBUG"))
                            std::cerr << "jlib::sys::Servent::start(): read command: " 
                                      << command << std::endl;

                        // WAKE is not looked up.  Its entire purpose was to
                        // end the poll above, and it has done that by being
                        // read; falling through would report it as an unmapped
                        // command every time somebody called add().
                        if(command != Servent::WAKE) {
                            std::lock_guard<std::mutex> lock(m_lock);
                            command_map_type::iterator i = m_commands.find(command);
                            if(i == m_commands.end()) {
                                throw exception("start(): cannot find signal for passed command");
                            }
                        
                            i->second();
                        }
                    }

                    std::lock_guard<std::mutex> lock(m_lock);
                    if(std::getenv("JLIB_SYS_SERVENT_DEBUG"))
                        std::cerr << "jlib::sys::Servent::start(): checking through: " 
                                  << m_conditions.size() << " conditions" << std::endl;
                    condition_list_type::iterator i = m_conditions.begin();
                    for(;i!=m_conditions.end();i++)
                        if(i->first())
                            i->second();
                    
                    cycle.emit();
                }
                catch(pipe::exception& e) {
                    // Fatal to the loop, where every other failure is not.
                    // The pipe is both how this is woken and how it is
                    // stopped, so a broken one cannot be retried around: with
                    // a blocking poll, "print it and go round again" is a
                    // tight spin on a descriptor that will fail identically
                    // forever.  It span at 1000/s before this change, which
                    // was survivable enough that nobody noticed.
                    std::cerr << "jlib::sys::Servent::start(): the command pipe failed, "
                              << "stopping: " << e.what() << std::endl;
                    m_bunny = false;
                }
                catch(std::exception& e) {
                    std::cerr << "jlib::sys::Servent::start(): caught std::exception: " << e.what() << std::endl;
                }
                catch(...) {
                    std::cerr << "jlib::sys::Servent::start(): caught unknown exception" << std::endl;
                }
            }
            
        }
        
    }
}
