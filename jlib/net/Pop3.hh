/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joe Yandle <jwy@divisionbyzero.com>
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

#ifndef JLIB_POP3_HH
#define JLIB_POP3_HH

#include <jlib/net/Email.hh>

#include <jlib/sys/socketstream.hh>

#include <jlib/util/URL.hh>

#include <exception>
#include <list>
#include <string>

namespace jlib {
    namespace net {

        /**
         * Class POP3 retrieves email from a POP3 server
         */
        class Pop3 {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = "jlib::net::Pop3::exception: "+msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };
        

            Pop3(jlib::util::URL url, bool remove=true);

            /**
             * Retrieve all email
             */
            std::list<std::string> retrieve();
            
        protected:
            std::string retrieve(jlib::sys::socketstream& sock, unsigned int which);
            
            void remove(jlib::sys::socketstream& sock,unsigned int which);
            
            /**
             * Connect to the server
             */
            jlib::sys::socketstream* connect();
            
            /**
             * Disconnect from the server
             */
            void disconnect(jlib::sys::socketstream& sock);
            
            std::string handshake(jlib::sys::socketstream& sock, std::string data, std::string ok);
            
            jlib::util::URL m_url;
            bool m_remove;
        };
        
    }
}
#endif //JLIB_POP3_HH
