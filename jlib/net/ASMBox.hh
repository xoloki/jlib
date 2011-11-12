/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joe Yandle <jwy@divisionbyzero.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
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

            virtual void on_set_password(std::string password);

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
