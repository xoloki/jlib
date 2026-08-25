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

#ifndef JLIB_NET_IMAP4FETCH_HH
#define JLIB_NET_IMAP4FETCH_HH

#include <jlib/net/Imap4.hh>
#include <jlib/net/MailFetch.hh>

namespace jlib {
    namespace net {

        class Imap4Fetch : public MailFetch, public Imap4 {
        public:
            /**
             * Create Imap4 with given username, password, and host
             *
             */
            Imap4Fetch(jlib::util::URL url, bool remove=false);
            
            /**
             * Destructor.
             */
            virtual ~Imap4Fetch();
            
            // MailFetch virtuals

            virtual std::vector<Email> retrieve();
        };
        
    }
}
#endif //JLIB_NET_IMAP4_HH
