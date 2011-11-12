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

#include <jlib/net/ASImapBox.hh>
#include <jlib/net/MailBox.hh>

#include <jlib/util/util.hh>
#include <jlib/util/Date.hh>

#include <algorithm>
#include <memory>

namespace jlib {
	namespace net {
        
        ASImapBox::ASImapBox(jlib::util::URL url, bool idle) 
            : Imap4(url),
              m_sock(0),
              m_idle(idle)
        {
        }

        
        void ASImapBox::on_init() {
            if(m_sock) {
                push(MailBoxResponse(MailBoxResponse::STATUS, "Closing old socket"));

                m_sock->close();
                delete m_sock;
                m_sock = 0;
                m_selected = folder_info_type();
            }

            util::URL url(m_url);
            url.set_pass("");
            
            push(MailBoxResponse(MailBoxResponse::STATUS, "Connecting to "+url()));
            m_sock = connect();
            
            push(MailBoxResponse(MailBoxResponse::STATUS, "Logging in to "+url()));
            try {
                login(*m_sock);
            } catch(std::exception& e) {
                push(MailBoxResponse(MailBoxResponse::LOGIN_ERROR));
                
                throw e;
            }
        }

        void ASImapBox::on_set_password(std::string password) {
            m_url.set_pass(password);
            m_pass = password;
        }

        void ASImapBox::on_list_folders() {
            push(MailBoxResponse(MailBoxResponse::STATUS, "Listing folders"));

            
            MailBoxResponse r(MailBoxResponse::FOLDER_LISTED);
            r.src.path = util::tokenize_list("INBOX");
            r.src.attr.is_parent = false;
            r.src.attr.is_select = true;
            this->push(r);

            std::list<std::string> path;
            list_subfolders(path);

            push(MailBoxResponse(MailBoxResponse::FOLDERS_LOADED));
            /*
            iterator i = find(util::tokenize_list("INBOX"));
            if(i == end())
                m_root.push_front(MailNode());
            */
        }

        void ASImapBox::on_create_folder(folder_info_type folder) {
            push(MailBoxResponse(MailBoxResponse::STATUS, "Creating folder " + 
                                 MailNode::pathstr(folder.path)));
            create(*m_sock,m_url.get_path_no_slash()+MailNode::pathstr(folder.path,m_delim,false));
            push(MailBoxResponse(MailBoxResponse::FOLDER_CREATED, folder));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done creating folder " + 
                                 MailNode::pathstr(folder.path)));
        }

        void ASImapBox::on_delete_folder(folder_info_type folder) {
            push(MailBoxResponse(MailBoxResponse::STATUS, "Deleting folder " + 
                                 MailNode::pathstr(folder.path)));
            remove(*m_sock,m_url.get_path_no_slash()+MailNode::pathstr(folder.path,m_delim,false));
            push(MailBoxResponse(MailBoxResponse::FOLDER_DELETED, folder));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done deleting folder " + 
                                 MailNode::pathstr(folder.path)));
        }

        void ASImapBox::on_rename_folder(folder_info_type src, folder_info_type dst) {
            push(MailBoxResponse(MailBoxResponse::STATUS, "Renaming folder " + 
                                 MailNode::pathstr(src.path) + " to " + MailNode::pathstr(dst.path)));
            rename(*m_sock,
                   m_url.get_path_no_slash()+MailNode::pathstr(src.path,m_delim,false),
                   m_url.get_path_no_slash()+MailNode::pathstr(dst.path,m_delim,false));
            push(MailBoxResponse(MailBoxResponse::FOLDER_RENAMED, src, dst));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done renaming folder " + 
                                 MailNode::pathstr(src.path) + " to " + MailNode::pathstr(dst.path)));
        }

        void ASImapBox::on_expunge_folder(folder_info_type folder) {
            push(MailBoxResponse(MailBoxResponse::STATUS, "Expunging folder " + 
                                 MailNode::pathstr(folder.path)));
            expunge(*m_sock);
            push(MailBoxResponse(MailBoxResponse::FOLDER_EXPUNGED, folder));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done expunging folder " + 
                                 MailNode::pathstr(folder.path)));
        }

        void ASImapBox::on_check_recent(folder_info_type folder) {
            // don't do this if we've switched folders
            if(m_selected != folder) {
                return;
            }

            std::string path;
            if(folder.path.size() == 1 && folder.path.front() == "INBOX") {
                path = "INBOX";
            } else {
                path = m_url.get_path_no_slash()+MailNode::pathstr(folder.path,m_delim,false);
            }

            push(MailBoxResponse(MailBoxResponse::STATUS, "Checking folder " + path + " for recent messages"));

            if(getenv("JLIB_NET_ASIMAP_DEBUG")) {
                std::cerr << "ASImapBox::on_check_recent: m_idle " << std::hex << m_idle << std::endl;
            }

            unsigned int old_exists = exists();
            unsigned int old_recent = recent();

            if(m_idle) 
                idle(*m_sock);
            else
                noop(*m_sock);

            unsigned int new_exists = exists();
            unsigned int new_recent = recent();


            if((new_recent > old_recent) || (new_exists > old_exists)) {
                unsigned int diff = (new_recent > old_recent) ? (new_recent - old_recent) : (new_exists - old_exists);

                push(MailBoxResponse(MailBoxResponse::STATUS, "Found " + util::string_value(diff) +
                                     " new messages in " + path + ", " + util::string_value(m_exists) + " total, " + util::string_value(m_unseen) + " unseen"));
                for(int i = (m_exists - diff); i < m_exists; i++) {
                    folder_indx_type sindx; sindx.push_back(i);
                    this->push(MailBoxRequest(MailBoxRequest::LIST_MESSAGES, folder, sindx));
                }
            } else {
                push(MailBoxResponse(MailBoxResponse::STATUS, "No new messages in folder " + path + ", " + util::string_value(m_exists) + " total, " + util::string_value(m_unseen) + " unseen"));
            }
        }

        void ASImapBox::on_list_messages(folder_info_type folder, folder_indx_type indx) {
            if(getenv("JLIB_NET_ASIMAP_DEBUG")) {
                std::cerr << "ASImapBox::on_list_messages: enter" << std::endl;
            }

            if(indx.empty()) {
                clear(MailBoxRequest::LIST_MESSAGES);
                clear(MailBoxResponse::MESSAGE_LISTED);
                push(MailBoxResponse(MailBoxResponse::STATUS, "Listing messages for folder " + 
                                     MailNode::pathstr(folder.path)));
                
                if(folder.path.size() == 1 && folder.path.front() == "INBOX") {
                    select(*m_sock, "INBOX");
                } else {
                    select(*m_sock, m_url.get_path_no_slash()+
                           MailNode::pathstr(folder.path,m_delim,false));
                }

                m_selected = folder;
                
                for(int i=0; i < m_exists; i++) {
                    folder_indx_type sindx;
                    sindx.push_back(i);

                    if(getenv("JLIB_NET_ASIMAP_DEBUG")) {
                        std::cerr << "ASImapBox::on_list_messages: push " << i << std::endl;
                    }
                    this->push(MailBoxRequest(MailBoxRequest::LIST_MESSAGES, folder, sindx));
                } 
                if(m_exists == 0) {
                    push(MailBoxResponse(MailBoxResponse::STATUS, "Done listing messages for folder " + 
                                         MailNode::pathstr(folder.path)));
                }

            } else {
                for(folder_indx_type::iterator i = indx.begin(); i != indx.end(); i++) {
                    if(getenv("JLIB_NET_ASIMAP_DEBUG")) {
                        std::cerr << "ASImapBox::on_list_messages: headers " << *i << std::endl;
                    }

                    /*
                    push(MailBoxResponse(MailBoxResponse::STATUS, "Listing message " + util::string_value(*i) +
                                             " for folder " + MailNode::pathstr(folder.path)));
                    */

                    MailBoxResponse r(MailBoxResponse::MESSAGE_LISTED, folder);
                    r.data = get(*m_sock, *i, true);
                    r.data.set_indx(*i);
                    r.indx.push_back(*i);

                    this->push(r);

                    if(*i == (m_exists-1)) {
                        push(MailBoxResponse(MailBoxResponse::STATUS, "Done listing messages for folder " + 
                                             MailNode::pathstr(folder.path)));
                    }
                }
            }
            if(getenv("JLIB_NET_ASIMAP_DEBUG")) {
                std::cerr << "ASImapBox::on_list_messages: leave" << std::endl;
            }

        }
        
        void ASImapBox::on_load_messages(folder_info_type folder, folder_indx_type indx) {
            if(m_selected != folder) {
                if(folder.path.size() == 1 && folder.path.front() == "INBOX") {
                    select(*m_sock, "INBOX");
                } else {
                    select(*m_sock, m_url.get_path_no_slash()+
                           MailNode::pathstr(folder.path,m_delim,false));
                }
                m_selected = folder;
            }

            folder_indx_type::iterator i = indx.begin();
            for(; i != indx.end(); i++) {
                this->push(MailBoxResponse(MailBoxResponse::STATUS,
                                           "Loading message " + util::string_value(*i)+ 
                                           " from folder " + MailNode::pathstr(folder.path)));

                MailBoxResponse r(MailBoxResponse::MESSAGE_LOADED, folder);
                r.data = get(*m_sock, *i);
                r.data.set_indx(*i);
                r.indx.push_back(*i);

                this->push(r);
            }

            this->push(MailBoxResponse(MailBoxResponse::STATUS,
                                       "Done loading messages from folder " + 
                                       MailNode::pathstr(folder.path)));

        }

        void ASImapBox::on_copy_messages(folder_info_type src, 
                                         folder_info_type dst, 
                                         folder_indx_type indx) {
            push(MailBoxResponse(MailBoxResponse::STATUS, 
                                 "Copying messages from " + MailNode::pathstr(src.path) + 
                                 " to " + MailNode::pathstr(dst.path)));
                
            
            folder_indx_type::iterator i = indx.begin();
            while(i != indx.end()) {
                folder_indx_type::iterator j = i; 
                folder_indx_type::iterator k = i; 

                k++;
                while(k != indx.end() && *k == *i) {
                    j = k;
                    k++;
                }
                
                copy(*m_sock, std::make_pair(*i+1, *j+1), MailNode::pathstr(dst.path,m_delim,false));

                i = ++j;
            }

            push(MailBoxResponse(MailBoxResponse::MESSAGES_COPIED, src, dst, indx));
            push(MailBoxResponse(MailBoxResponse::STATUS, 
                                 "Done copying messages from " + MailNode::pathstr(src.path) + 
                                 " to " + MailNode::pathstr(dst.path)));
        }

        
        void ASImapBox::on_append_message(folder_info_type folder, Email email) {
            push(MailBoxResponse(MailBoxResponse::STATUS, "Appending to folder " + 
                                 MailNode::pathstr(folder.path)));
            append(*m_sock, MailNode::pathstr(folder.path, m_delim, false), email.raw());
            push(MailBoxResponse(MailBoxResponse::MESSAGE_APPENDED, folder));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done appending to folder " + 
                                 MailNode::pathstr(folder.path)));
            
        }

        void ASImapBox::on_set_message_flags(folder_info_type folder, 
                                             folder_indx_type indx, 
                                             Email email) {
            std::vector<std::string> flagv;
            std::set<Email::flag_type> flags = email.get_flags();
            std::string flagstr;
            if(flags.find(Email::answered_flag) != flags.end()) {
                flagv.push_back("\\Answered");
                flagstr += "\\Answered";
            }
            if(flags.find(Email::seen_flag) != flags.end()) {
                flagv.push_back("\\Seen");
                flagstr += "\\Seen";
            }
            if(flags.find(Email::deleted_flag) != flags.end()) {
                flagv.push_back("\\Deleted");
                flagstr += "\\Deleted";
            }

            push(MailBoxResponse(MailBoxResponse::STATUS, "Setting flags " + flagstr +
                                 " on folder " + MailNode::pathstr(folder.path)));

            folder_sort_indx_type sort_indx;
            for(folder_indx_type::iterator i = indx.begin(); i != indx.end(); i++) {
                sort_indx.insert(sort_indx.begin(), *i);
            }

            folder_sort_indx_type::iterator i = sort_indx.begin();

            while(i != sort_indx.end()) {
                folder_sort_indx_type::iterator j = i; 
                folder_sort_indx_type::iterator k = i; 

                k++;
                while(k != sort_indx.end() && *k == (*j + 1)) {
                    j = k;
                    k++;
                }
                
                store(*m_sock, std::make_pair<u_int,u_int>(*i+1, *j+1), "+FLAGS.SILENT", flagv);

                i = ++j;
            }

            push(MailBoxResponse(MailBoxResponse::MESSAGE_FLAGS_SET, folder, email, indx));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done setting flags " + flagstr +
                                 " on folder " + MailNode::pathstr(folder.path)));
        }
        
        void ASImapBox::on_unset_message_flags(folder_info_type folder, 
                                            folder_indx_type indx, 
                                            Email email) {
            std::vector<std::string> flagv;
            std::set<Email::flag_type> flags = email.get_flags();
            std::string flagstr;
            if(flags.find(Email::answered_flag) != flags.end()) {
                flagv.push_back("\\Answered");
                flagstr += "\\Answered";
            }
            if(flags.find(Email::seen_flag) != flags.end()) {
                flagv.push_back("\\Seen");
                flagstr += "\\Seen";
            }
            if(flags.find(Email::deleted_flag) != flags.end()) {
                flagv.push_back("\\Deleted");
                flagstr += "\\Deleted";
            }

            push(MailBoxResponse(MailBoxResponse::STATUS, "Unsetting flags " + flagstr +
                                 " on folder " + MailNode::pathstr(folder.path)));


            folder_sort_indx_type sort_indx;
            for(folder_indx_type::iterator i = indx.begin(); i != indx.end(); i++) {
                sort_indx.insert(sort_indx.begin(), *i);
            }

            folder_sort_indx_type::iterator i = sort_indx.begin();

            while(i != sort_indx.end()) {
                folder_sort_indx_type::iterator j = i; 
                folder_sort_indx_type::iterator k = i; 

                k++;
                while(k != sort_indx.end() && *k == (*j + 1)) {
                    j = k;
                    k++;
                }
                
                store(*m_sock, std::make_pair<u_int,u_int>(*i+1, *j+1), "-FLAGS.SILENT", flagv);

                i = ++j;
            }

            push(MailBoxResponse(MailBoxResponse::MESSAGE_FLAGS_UNSET, folder, email, indx));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done unsetting flags " + flagstr +
                                 " on folder " + MailNode::pathstr(folder.path)));
        }

        void ASImapBox::list_subfolders(std::list<std::string> path) {
            if(getenv("JLIB_NET_IMAP4_DEBUG"))
                std::cout << "void ASImapBox::list_subfolders(" << path.size() << " nodes in path)" << std::endl;
            
            std::string delim, name;
            std::string pathstr = m_url.get_path_no_slash() +
                MailNode::pathstr(path,m_delim,false,true,false);

            if(getenv("JLIB_NET_IMAP4_DEBUG"))
                std::cout << "void ASImapBox::list_subfolders: pathstr" << pathstr <<std::endl;
            
            std::vector<ListItem> ls = Imap4::list(*m_sock,"",pathstr+"%");
            if(ls.size() >= 1) {
                for(u_int i=0;i<ls.size();i++) {
                    name = ls[i].get_name();
                    delim = ls[i].get_delim();

                    if(getenv("JLIB_NET_IMAP4_DEBUG"))
                        std::cout << "void ASImapBox::list_subfolders: found item " << name <<std::endl;

                    if(delim != "" && delim != "NIL" && m_delim != delim) {
                        if(getenv("JLIB_NET_IMAP4_DEBUG"))
                            std::cout << "void ASImapBox::list_subfolders: setting m_delim to " << delim <<std::endl;
                        m_delim = delim;
                    }

                    if(name != pathstr) {
                        std::list<std::string> cpath = jlib::util::tokenize_list(name,delim);
                        
                        path.push_back(cpath.back());

                        MailBoxResponse r(MailBoxResponse::FOLDER_LISTED);
                        r.src.path = path;
                        r.src.attr.is_parent = ls[i].is_parent();
                        r.src.attr.is_select = ls[i].is_folder();
                        this->push(r);

                        if(ls[i].is_parent()) {
                            list_subfolders(path);
                        }
                        path.pop_back();
                    }
                }
            }

        }

    }
}

