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

#ifndef JLIB_NET_ASIMAPBOX_HH
#define JLIB_NET_ASIMAPBOX_HH

#include <string>
#include <iostream>
#include <map>
#include <vector>

#include <jlib/net/ASMailBox.hh>
#include <jlib/net/Imap4.hh>

namespace jlib {
	namespace net {
        
        class ASImapBox : public Imap4, public ASMailBox {
        public:
            ASImapBox(jlib::util::URL url, bool idle = false);

            virtual void on_init();

            virtual void on_set_password(const std::string& password);

            virtual void on_list_folders();

            virtual void on_create_folder(folder_info_type folder);
            virtual void on_delete_folder(folder_info_type folder);
            virtual void on_rename_folder(folder_info_type src, folder_info_type dst);
            virtual void on_expunge_folder(folder_info_type folder);

            virtual void on_check_recent(folder_info_type folder);

            virtual void on_list_messages(folder_info_type folder, folder_indx_type indx);
            virtual void on_load_messages(folder_info_type folder, folder_indx_type indx);
            virtual void on_copy_messages(folder_info_type src, folder_info_type dst, 
                                          folder_indx_type indx);

            virtual void on_append_message(folder_info_type folder, Email email);

            virtual void on_set_message_flags(folder_info_type folder, 
                                              folder_indx_type indx, Email email);
            virtual void on_unset_message_flags(folder_info_type folder, 
                                                folder_indx_type indx, Email email);
        protected:
            void list_subfolders(std::list<std::string> path);

            folder_info_type m_selected;
            sys::socketstream* m_sock;

            bool m_idle;
        };
    }
}

#endif //JLIB_NET_ASIMAPBOX
