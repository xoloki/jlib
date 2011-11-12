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

#ifndef JLIB_SYS_SERVENT_HH
#define JLIB_SYS_SERVENT_HH

#include <glibmm/thread.h>
#include <jlib/sys/pipe.hh>
#include <jlib/sys/object.hh>
#include <jlib/sys/sync.hh>

#include <exception>
#include <string>
#include <sstream>
#include <map>

#include <cstring>

#include <sigc++/sigc++.h>

#include <errno.h>

namespace jlib {
    namespace sys {

        class Servent : public Object {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = "jlib::sys::Servent exception"+
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

            typedef int                                       id_type;
            typedef std::map< id_type,sigc::slot<void> >     command_map_type;

            typedef std::list<std::pair<sigc::slot<bool>,sigc::slot<void> > >
                                                              condition_list_type;

            static const id_type EXIT = -1;

            Servent();
            virtual ~Servent();

            void map(id_type command, sigc::slot<void> slot);
            void add(condition_list_type::value_type condition);

            void exec(id_type command, int maxwait=-1);

            void run();
            
            void start();

            sigc::signal0<void> cycle;

        protected:
            pipe m_pipe;
            command_map_type m_commands;
            condition_list_type m_conditions;
            Glib::Thread* m_worker;
            Glib::Mutex m_lock;
            sync<bool> m_bunny;
        };

    }
}

#endif //JLIB_SYS_SERVENT_HH
