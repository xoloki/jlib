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
            std::lock_guard<std::mutex> lock(m_lock);
            m_conditions.push_back(condition);
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
                    if(m_pipe.poll(pipe::IN, 1)) {
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
                        std::lock_guard<std::mutex> lock(m_lock);
                        command_map_type::iterator i = m_commands.find(command);
                        if(i == m_commands.end()) {
                            throw exception("start(): cannot find signal for passed command");
                        }
                        
                        i->second();
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
