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

#include <jlib/net/net.hh>
#include <jlib/net/Imap4Fetch.hh>

#include <jlib/sys/sys.hh>

#include <jlib/util/util.hh>

#include <sstream>
#include <memory>

namespace jlib {
    namespace net {
        
        Imap4Fetch::Imap4Fetch(jlib::util::URL url, bool remove) 
            : MailFetch(url,remove),
              Imap4(url)
        {
            
        }

        Imap4Fetch::~Imap4Fetch() {
            
        }
        
        std::vector<Email> Imap4Fetch::retrieve() {
            std::vector<Email> ret;
            std::unique_ptr<jlib::sys::socketstream> sock(connect());
            login(*sock);
            select(*sock,"INBOX");

            for(unsigned int i=1; i<=m_exists; i++) {
                std::string data = this->Imap4::retrieve(i);
                ret.push_back(Email(data));
            }

            logout(*sock);
            disconnect(*sock);

            return ret;
        }


    }
}
