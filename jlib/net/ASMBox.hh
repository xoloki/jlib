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

#ifndef JLIB_NET_ASMBOX_HH
#define JLIB_NET_ASMBOX_HH

#include <string>
#include <iostream>
#include <map>
#include <vector>

#include <jlib/net/ASMailBox.hh>
#include <jlib/util/URL.hh>

namespace jlib {
	namespace net {
        
        class ASMBox : public ASMailBox {
        public:
            ASMBox(jlib::util::URL url);

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
            void make_selected(folder_info_type folder);
            void reset_stream(folder_info_type folder);
            void check_stream();

            folder_info_type m_selected;

            util::URL m_url;

            std::string m_inbox;
            std::string m_maildir;
            bool m_canonical;
            std::vector<long> m_divide;
            std::istream* m_is;
        };
    }
}

#endif //JLIB_NET_ASMBOX
