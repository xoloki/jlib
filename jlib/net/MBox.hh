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

#ifndef JLIB_NET_MBOX_HH
#define JLIB_NET_MBOX_HH

#include <jlib/net/MailBox.hh>

#include <jlib/util/URL.hh>

#include <string>
#include <iostream>
#include <map>
#include <vector>

namespace jlib {
	namespace net {
        
        
        class MBoxBuf : public BoxBuf {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = std::string("jlib::net::MBoxBuf exception")+( (msg=="")?"":": ")+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            MBoxBuf(jlib::util::URL url);

            virtual void list();

            virtual void fill(std::list<std::string> path);

            virtual void create_folder(std::list<std::string> path);
            virtual void delete_folder(std::list<std::string> path);
            virtual void rename_folder(std::list<std::string> path, std::list<std::string> npath);

            bool is_inbox(std::list<std::string> path);
        protected:
            void tree(std::list<std::string> path, reference root);

            std::string m_inbox;
            std::string m_maildir;
            bool m_canonical;
        };

        class MBox : public MailBox {
        public:
            MBox(jlib::util::URL url)
                : MailBox(0)
            {
                m_buf = new MBoxBuf(url);
            }
        };

        
    }
}

#endif //JLIB_NET_MAILBOX_HH
