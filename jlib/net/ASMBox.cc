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

#include <jlib/net/ASMBox.hh>
#include <jlib/net/net.hh>
#include <jlib/net/MailBox.hh>

#include <jlib/util/util.hh>
#include <jlib/util/Date.hh>

#include <jlib/sys/Directory.hh>

#include <algorithm>
#include <memory>

#include <paths.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

const std::string SYS_MAIL_DIR = std::string(_PATH_MAILDIR);

namespace jlib {
	namespace net {
        
        ASMBox::ASMBox(jlib::util::URL url) 
            : m_url(url),
              m_is(0)
        {
            
        }

        
        void ASMBox::on_init() {
            push(MailBoxResponse(MailBoxResponse::STATUS, "Initializing"));
            if(m_url.get_path() == "") {
                m_maildir = std::string(getenv("HOME"))+std::string("/mail");
            }
            else {
                m_maildir = m_url.get_path();
            }
            if(m_url["canonical"] != "false") {
                m_inbox = SYS_MAIL_DIR+std::string("/")+std::string(getenv("USER"));
                m_canonical = true;
            }
            else {
                m_canonical = false;
            }
            push(MailBoxResponse(MailBoxResponse::STATUS, "Initialized"));
        }

        void ASMBox::on_set_password(std::string password) {

        }

        void ASMBox::on_list_folders() {
            push(MailBoxResponse(MailBoxResponse::STATUS, "Listing folders"));

            
            MailBoxResponse r(MailBoxResponse::FOLDER_LISTED);
            r.src.path = util::tokenize_list("INBOX");
            r.src.attr.is_parent = false;
            r.src.attr.is_select = true;
            this->push(r);

            std::list<std::string> path;
            list_subfolders(path);

            push(MailBoxResponse(MailBoxResponse::FOLDERS_LOADED));
        }

        void ASMBox::on_create_folder(folder_info_type folder) {
            push(MailBoxResponse(MailBoxResponse::STATUS, "Creating folder " + 
                                 MailNode::pathstr(folder.path)));

            mbox::create(m_maildir, folder.path);

            push(MailBoxResponse(MailBoxResponse::FOLDER_CREATED, folder));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done creating folder " + 
                                 MailNode::pathstr(folder.path)));
        }

        void ASMBox::on_delete_folder(folder_info_type folder) {
            std::string pathstr = MailNode::pathstr(folder.path);
            push(MailBoxResponse(MailBoxResponse::STATUS, "Deleting folder " + pathstr));
            
            mbox::deleet(m_maildir, folder.path);
            
            push(MailBoxResponse(MailBoxResponse::FOLDER_DELETED, folder));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done deleting folder " + pathstr));
        }
        
        void ASMBox::on_rename_folder(folder_info_type src, folder_info_type dst) {
            std::string srcstr = m_maildir + MailNode::pathstr(src.path);
            std::string dststr = m_maildir + MailNode::pathstr(dst.path);
            
            push(MailBoxResponse(MailBoxResponse::STATUS, "Renaming folder " + srcstr + " to " + dststr));
            
            mbox::rename(m_maildir, src.path, dst.path);

            push(MailBoxResponse(MailBoxResponse::FOLDER_RENAMED, src, dst));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done renaming folder " + srcstr + " to " + dststr));
        }
        
        void ASMBox::on_expunge_folder(folder_info_type folder) {
            push(MailBoxResponse(MailBoxResponse::STATUS, "Expunging folder " + 
                                 MailNode::pathstr(folder.path)));

            std::list<int> del;
            {
                sys::auto_lock<std::mutex> lock(m_folders);

                folder_type& rep_folder = m_folders.ref()[folder.path];
                for(folder_data_type::iterator i = rep_folder.data.begin(); i != rep_folder.data.end(); i++) {
                    std::set<Email::flag_type> flags = i->second.get_flags();
                    if(flags.find(Email::deleted_flag) != flags.end()) {
                        del.push_back(i->first);
                    }
                }
            }

            mbox::remove(m_maildir, folder.path, del, m_divide);
            reset_stream(folder);

            push(MailBoxResponse(MailBoxResponse::FOLDER_EXPUNGED, folder));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done expunging folder " + 
                                 MailNode::pathstr(folder.path)));
        }

        void ASMBox::on_check_recent(folder_info_type folder) {
            // don't do this if we've switched folders
            if(m_selected != folder) {
                return;
            }

            std::string path = mbox::make_path(m_maildir, folder.path);

            push(MailBoxResponse(MailBoxResponse::STATUS, "Checking folder " + path + " for recent messages"));
            bool change = false;
            {
                sys::auto_lock<std::mutex> lock(m_folders);
                long mtime;
                folder_type& rep_folder = m_folders.ref()[folder.path];
                if((mtime=util::file::mtime(path)) != rep_folder.info.attr.mtime) {
                    change = true;
                }
            }

            if(change) {
                push(MailBoxResponse(MailBoxResponse::STATUS, "Found new messages in " + path));
                this->push(MailBoxRequest(MailBoxRequest::LIST_MESSAGES, folder));
            }
            else {
                push(MailBoxResponse(MailBoxResponse::STATUS, "No new messages in folder " + path));
            }
        }

        void ASMBox::on_list_messages(folder_info_type folder, folder_indx_type indx) {
            if(getenv("JLIB_NET_ASM_DEBUG")) {
                std::cerr << "ASMBox::on_list_messages: enter" << std::endl;
            }

            std::string path = mbox::make_path(m_maildir, folder.path);

            if(getenv("JLIB_NET_ASM_DEBUG")) {
                std::cerr << "ASMBox::on_list_messages: listing " << path << std::endl;
            }

            make_selected(folder);

            if(indx.empty()) {

                clear(MailBoxRequest::LIST_MESSAGES);
                clear(MailBoxResponse::MESSAGE_LISTED);

                m_divide.clear();
                {
                    sys::auto_lock<std::mutex> lock(m_folders);
                    folder_type& rep_folder = m_folders.ref()[folder.path];
                    rep_folder.info.attr.mtime = util::file::mtime(path);
                    rep_folder.data.clear();
                }

                push(MailBoxResponse(MailBoxResponse::STATUS, "Listing messages for folder " + path));

                parse_divide(*m_is, m_divide, "From ");
                
                m_selected = folder;
                
                for(int i=0; i < m_divide.size(); i++) {
                    folder_indx_type sindx;
                    sindx.push_back(i);

                    if(getenv("JLIB_NET_ASM_DEBUG")) {
                        std::cerr << "ASMBox::on_list_messages: push " << i << std::endl;
                    }
                    this->push(MailBoxRequest(MailBoxRequest::LIST_MESSAGES, folder, sindx));
                } 
                if(m_divide.size() == 0) {
                    push(MailBoxResponse(MailBoxResponse::STATUS, "Done listing messages for folder " + 
                                         MailNode::pathstr(folder.path)));
                }

            } else {
                for(folder_indx_type::iterator i = indx.begin(); i != indx.end(); i++) {
                    if(getenv("JLIB_NET_ASM_DEBUG")) {
                        std::cerr << "ASMBox::on_list_messages: headers " << *i << std::endl;
                    }

                    MailBoxResponse r(MailBoxResponse::MESSAGE_LISTED, folder);
                    r.data = mbox::get(*m_is, *i, m_divide, true);
                    r.data.set_indx(*i);
                    r.indx.push_back(*i);

                    if(*i == 0 && r.data["subject"] == "DON'T DELETE THIS MESSAGE -- FOLDER INTERNAL DATA") {
                        // don't push the internal mail
                    } else {
                        this->push(r);
                    }

                    if(*i == (m_divide.size()-1)) {
                        push(MailBoxResponse(MailBoxResponse::STATUS, "Done listing messages for folder " + 
                                             MailNode::pathstr(folder.path)));
                    }
                }
            }
            if(getenv("JLIB_NET_ASM_DEBUG")) {
                std::cerr << "ASMBox::on_list_messages: leave" << std::endl;
            }

        }
        
        void ASMBox::on_load_messages(folder_info_type folder, folder_indx_type indx) {
            std::string path = mbox::make_path(m_maildir, folder.path);

            make_selected(folder);

            folder_indx_type::iterator i = indx.begin();
            for(; i != indx.end(); i++) {
                this->push(MailBoxResponse(MailBoxResponse::STATUS,
                                           "Loading message " + util::string_value(*i)+ 
                                           " from folder " + MailNode::pathstr(folder.path)));

                MailBoxResponse r(MailBoxResponse::MESSAGE_LOADED, folder);
                r.data = mbox::get(*m_is, *i, m_divide);
                r.data.set_indx(*i);
                r.data.set_flag(Email::seen_flag);
                r.indx.push_back(*i);

                this->push(r);
            }

            this->push(MailBoxResponse(MailBoxResponse::STATUS,
                                       "Done loading messages from folder " + 
                                       MailNode::pathstr(folder.path)));

        }

        void ASMBox::on_copy_messages(folder_info_type src, 
                                      folder_info_type dst, 
                                      folder_indx_type indx) {
            push(MailBoxResponse(MailBoxResponse::STATUS, 
                                 "Copying messages from " + MailNode::pathstr(src.path) + 
                                 " to " + MailNode::pathstr(dst.path)));
            
            make_selected(src);
            
            folder_indx_type::iterator i = indx.begin();
            for(; i != indx.end(); i++) {
                mbox::append(m_maildir, dst.path, mbox::get(*m_is, *i, m_divide));
            }

            push(MailBoxResponse(MailBoxResponse::MESSAGES_COPIED, src, dst, indx));
            push(MailBoxResponse(MailBoxResponse::STATUS, 
                                 "Done copying messages from " + MailNode::pathstr(src.path) + 
                                 " to " + MailNode::pathstr(dst.path)));
        }

        
        void ASMBox::on_append_message(folder_info_type folder, Email email) {
            push(MailBoxResponse(MailBoxResponse::STATUS, "Appending to folder " + 
                                 MailNode::pathstr(folder.path)));

            mbox::append(m_maildir, folder.path, email);

            push(MailBoxResponse(MailBoxResponse::MESSAGE_APPENDED, folder));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done appending to folder " + 
                                 MailNode::pathstr(folder.path)));
            
        }

        void ASMBox::on_set_message_flags(folder_info_type folder, 
                                          folder_indx_type indx, 
                                          Email email) {
            std::set<Email::flag_type> flags = email.get_flags();
            
            push(MailBoxResponse(MailBoxResponse::STATUS, "Setting flags on folder " + 
                                 MailNode::pathstr(folder.path)));
                                 

            folder_indx_type::iterator i = indx.begin();
            for(; i != indx.end(); i++) {
                sys::auto_lock<std::mutex> lock(m_folders);
                folder_type& rep_folder = m_folders.ref()[folder.path];

                rep_folder.data[*i].set_flags(flags);
            }

            push(MailBoxResponse(MailBoxResponse::MESSAGE_FLAGS_SET, folder, email, indx));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done setting flags on folder " + 
                                 MailNode::pathstr(folder.path)));
        }
        
        void ASMBox::on_unset_message_flags(folder_info_type folder, 
                                            folder_indx_type indx, 
                                            Email email) {
            std::set<Email::flag_type> flags = email.get_flags();
            
            push(MailBoxResponse(MailBoxResponse::STATUS, "Setting flags on folder " + 
                                 MailNode::pathstr(folder.path)));
                                 

            folder_indx_type::iterator i = indx.begin();
            for(; i != indx.end(); i++) {
                sys::auto_lock<std::mutex> lock(m_folders);
                folder_type& rep_folder = m_folders.ref()[folder.path];

                rep_folder.data[*i].unset_flags(flags);
            }

            push(MailBoxResponse(MailBoxResponse::MESSAGE_FLAGS_SET, folder, email, indx));
            push(MailBoxResponse(MailBoxResponse::STATUS, "Done setting flags on folder " + 
                                 MailNode::pathstr(folder.path)));

        }

        void ASMBox::list_subfolders(std::list<std::string> path) {
            std::string pathstr = m_maildir + MailNode::pathstr(path);
            try {
                jlib::sys::Directory dir(pathstr);
                std::vector<std::string> ls = dir.list();
                bool folder,parent;
                
                if(ls.size() >= 1) {
                    for(u_int i=0;i<ls.size();i++) {
                        path.push_back(ls[i]);

                        folder = dir.is(ls[i], jlib::sys::REGULAR);
                        parent = dir.is(ls[i], jlib::sys::DIRECTORY);

                        MailBoxResponse r(MailBoxResponse::FOLDER_LISTED);
                        r.src.path = path;
                        r.src.attr.is_parent = parent;
                        r.src.attr.is_select = folder;
                        this->push(r);

                        if(parent) {
                            list_subfolders(path);
                        }

                        path.pop_back();
                    }
                }
            } 
            catch(std::exception& e) {
                push(MailBoxResponse(MailBoxResponse::ERROR, "Error listing subfolders of " + pathstr + ": " + e.what()));
            } 
            catch(...) {
                push(MailBoxResponse(MailBoxResponse::ERROR, "Error listing subfolders of " + pathstr));
            }
            
        }

        void ASMBox::make_selected(folder_info_type folder) {
            std::string path = mbox::make_path(m_maildir, folder.path);
            if(m_selected != folder) {
                reset_stream(folder);
                check_stream();

                m_selected = folder;
            }

            if(m_is && m_is->eof()) {
                if(getenv("JLIB_NET_ASM_DEBUG")) {
                    std::cerr << "ASMBox::make_selected: eof(), seekg(0)" << std::endl;
                }
                m_is->seekg(0, std::ios_base::beg);
                m_is->clear();
            }

            check_stream();
        }

        void ASMBox::reset_stream(folder_info_type folder) {
            std::string path = mbox::make_path(m_maildir, folder.path);
            if(m_is) {
                delete m_is;
                m_is = 0;
            }
            
            m_is = new std::ifstream(path.c_str(), std::ios::in);
        }

        void ASMBox::check_stream() {
            if(!m_is) {
                throw exception("ASMBox: null stream");
            }
            if(m_is->fail()) {
                delete m_is;
                m_is = 0;
                throw exception("ASMBox: failed stream");
            }
            if(m_is->bad()) {
                delete m_is;
                m_is = 0;
                throw exception("ASMBox: bad stream");
            }
            
        }

    }
}

