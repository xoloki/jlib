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

#ifndef JLIB_NET_IMAP4BOX_HH
#define JLIB_NET_IMAP4BOX_HH

#include <string>
#include <iostream>
#include <map>
#include <vector>

#include <jlib/net/MailBox.hh>
#include <jlib/net/Imap4.hh>

namespace jlib {
	namespace net {
        
        class Imap4BoxBuf : public Imap4, public BoxBuf {
        public:
            Imap4BoxBuf(jlib::util::URL url);

            virtual void list();

            virtual void fill(std::list<std::string> path);

            virtual void create_folder(std::list<std::string> path);
            virtual void delete_folder(std::list<std::string> path);
            virtual void rename_folder(std::list<std::string> path, std::list<std::string> npath);

        protected:
            void tree(std::list<std::string> path, jlib::sys::socketstream& sock, reference root);
        };

        class Imap4Box : public MailBox {
        public:
            Imap4Box(jlib::util::URL url)
                : MailBox(0)
            {
                m_buf = new Imap4BoxBuf(url);
            }
        };
        
    }
}

#endif //JLIB_NET_MAILGROUP_HH
