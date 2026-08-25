/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
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
                exception(const std::string& msg = "") {
                    m_msg = "jlib::net::Pop3::exception: "+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };
        

            Pop3(jlib::util::URL url, bool remove=true);

            /**
             * Retrieve all email
             */
            std::list<std::string> retrieve();

            /**
             * Read a multi-line response body, up to the "." that ends it.
             *
             * RFC 1939 3: a multi-line response ends with CRLF "." CRLF, and
             * nothing else terminates it.  The dot transparency is undone on
             * the way past, so what comes back is the message as it was sent.
             *
             * Public and static because it is the whole of what is worth
             * testing here, and a std::istringstream is a POP3 server for the
             * purpose.
             *
             * Throws Pop3::exception if the stream ends before the "." does --
             * a truncated message is not a short message.
             */
            static std::string read_body(std::istream& is);

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
            
            std::string handshake(jlib::sys::socketstream& sock, const std::string& data, const std::string& ok);
            
            jlib::util::URL m_url;
            bool m_remove;
        };
        
    }
}
#endif //JLIB_POP3_HH
