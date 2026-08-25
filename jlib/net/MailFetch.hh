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

#ifndef JLIB_NET_MAILFETCH_HH
#define JLIB_NET_MAILFETCH_HH

#include <jlib/net/net.hh>

#include <jlib/sys/object.hh>

#include <jlib/util/URL.hh>

#include <vector>

namespace jlib {
    namespace net {


        /**
         * Class Fetch retrieves email from a server
         */
        class MailFetch : public sys::Object {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = "jlib::net::MailFetch exception: "+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };
        
            
            MailFetch() {}
            MailFetch(jlib::util::URL url) { m_url = url; }
            MailFetch(jlib::util::URL url, u_int time) { m_url = url; m_time = time; }
            virtual ~MailFetch() {}
            /**
             * Retrieve all the mail, deleting as we go
             */
            virtual std::vector<Email> retrieve()=0;

            jlib::util::URL get_url() const { return m_url; }
            void set_url(jlib::util::URL url) { m_url = url; }

            u_int get_time() const { return m_time; }
            void set_time(u_int time) { m_time = time; }
        protected:
            jlib::util::URL m_url;
            u_int m_time;
        };
        
    }
}
#endif
