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

#ifndef JLIB_NET_IMAP4FOLDER_HH
#define JLIB_NET_IMAP4FOLDER_HH

#include <jlib/net/MailBox.hh>
#include <jlib/net/Imap4.hh>

namespace jlib {
	namespace net {
        
        class Imap4FolderBuffer : public FolderBuffer, public Imap4 {
        public:
            Imap4FolderBuffer(jlib::util::URL url);

            virtual ~Imap4FolderBuffer();
            
            virtual bool modified();
            virtual void scan(bool check_modified=false);

            virtual void set_flags(std::set<Email::flag_type> flags, std::list<unsigned int> which);
            virtual void unset_flags(std::set<Email::flag_type> flags, std::list<unsigned int> which);
            virtual void sync();
            virtual void fill(std::list<unsigned int> which);

            virtual void add(std::vector<Email> mails);
            

        protected:
            std::string m_path;
            bool m_scanned;
        };
        
        class Imap4Folder : public MailFolder {
        public:
            Imap4Folder(jlib::util::URL url)
                : MailFolder(NULL)
            {
                // Straight to init, which owns it; see MFolder.
                init(new Imap4FolderBuffer(url));
            }
        };

    }
}

#endif //JLIB_NET_IMAP4BOX_HH
