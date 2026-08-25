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

#ifndef JLIB_NET_MFOLDER_HH
#define JLIB_NET_MFOLDER_HH

#include <jlib/net/MailBox.hh>

namespace jlib {
    namespace net {
        
        class MFolderBuffer : public FolderBuffer {
        public:
            MFolderBuffer(const std::string& path);

            virtual ~MFolderBuffer();
            
            virtual bool modified();
            virtual void scan(bool check_modified=false);

            virtual void set_flags(std::set<Email::flag_type> flags, std::list<unsigned int> which);
            virtual void unset_flags(std::set<Email::flag_type> flags, std::list<unsigned int> which);
            virtual void sync();
            virtual void fill(std::list<unsigned int> which);

            virtual void add(std::vector<Email> mails);

        protected:
            void scan_divide();
            void scan_headers();
            void remove(std::list<unsigned int> which);

            /**
             * the file this box uses
             */
            std::string m_path;

            /**
             * where the physical boundaries are between the emails
             */
            std::vector<long> m_divide;

            /**
             * when we began the last scan
             */
            time_t m_scan_begin;
        };
        
        class MFolder : public MailFolder {
        public:
            MFolder(const std::string& path)
                : MailFolder(NULL)
            {
                // Straight to init, which owns it.  This assigned the base's
                // m_rep and then passed the same pointer to init, which
                // assigned it again.
                init(new MFolderBuffer(path));
            }
                
        };
        
    }
}

#endif //NET_MBOX_HH
