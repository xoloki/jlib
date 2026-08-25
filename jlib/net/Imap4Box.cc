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

#include <jlib/net/Imap4Box.hh>
#include <jlib/net/Imap4Folder.hh>

#include <jlib/sys/Directory.hh>

#include <jlib/util/util.hh>
#include <jlib/util/Date.hh>

#include <algorithm>
#include <memory>

namespace jlib {
	namespace net {
        
        Imap4BoxBuf::Imap4BoxBuf(jlib::util::URL url) 
            : Imap4(url)
        {
            
        }
        
        void Imap4BoxBuf::list() {
            std::list<std::string> path;// = util::tokenize_list(m_url.get_path());
            std::unique_ptr<jlib::sys::socketstream> sock(connect());

            login(*sock);
            tree(path,*sock,m_root);
            logout(*sock);
            disconnect(*sock);

            iterator i = find(util::tokenize_list("INBOX"));
            if(i == end())
                m_root.push_front(MailNode());
        }

        void Imap4BoxBuf::fill(std::list<std::string> path) {
            iterator i = find(path);
            if(i != end() && !i->is_filled()) {
                jlib::util::URL url(m_url);
                if(path.size() == 1 && util::upper(path.front()) == "INBOX")
                    url.set_path("INBOX");
                else
                    url.set_path(m_url.get_path()+MailNode::pathstr(path));
                if(m_delim != "" && m_delim != "NIL")
                    url.set_delim(m_delim);
                i->set_folder(new Imap4Folder(url));
            }
        }

        void Imap4BoxBuf::create_folder(std::list<std::string> path) {
            std::unique_ptr<jlib::sys::socketstream> sock(connect());
            login(*sock);
            
            create(*sock,m_url.get_path_no_slash()+MailNode::pathstr(path,m_delim,false));

            logout(*sock);
            disconnect(*sock);
        }

        void Imap4BoxBuf::delete_folder(std::list<std::string> path) {
            std::unique_ptr<jlib::sys::socketstream> sock(connect());
            login(*sock);
            
            remove(*sock,m_url.get_path_no_slash()+MailNode::pathstr(path,m_delim,false));

            logout(*sock);
            disconnect(*sock);
        }

        void Imap4BoxBuf::rename_folder(std::list<std::string> path, std::list<std::string> npath) {
            std::unique_ptr<jlib::sys::socketstream> sock(connect());
            login(*sock);

            rename(*sock,
                   m_url.get_path_no_slash()+MailNode::pathstr(path,m_delim,false),
                   m_url.get_path_no_slash()+MailNode::pathstr(npath,m_delim,false));

            logout(*sock);
            disconnect(*sock);
        }

        void Imap4BoxBuf::tree(std::list<std::string> path, 
                               jlib::sys::socketstream& sock, reference root) {
            if(getenv("JLIB_NET_IMAP4_DEBUG"))
                std::cout << "void Imap4BoxBuf::tree(list<"
                          <<path.size()<<">,socketstream&,MailNode(\""
                          <<root.get_name()<<"\")"<<std::endl;
            
            std::string delim, name;
            std::string pathstr = m_url.get_path_no_slash() +
                MailNode::pathstr(path,m_delim,false,true,false);
                
            
            std::vector<ListItem> ls = Imap4::list(sock,"",pathstr+"%");
            if(ls.size() >= 1) {
                for(u_int i=0;i<ls.size();i++) {
                    name = ls[i].get_name();
                    delim = ls[i].get_delim();

                    if(delim != "" && delim != "NIL" && m_delim != delim) {
                        m_delim = delim;
                    }

                    if(name != pathstr) {
                        std::list<std::string> cpath = jlib::util::tokenize_list(name,delim);
                        
                        path.push_back(cpath.back());
                        root.push_back(MailNode(path,ls[i].is_folder(),ls[i].is_parent()));
                        if(ls[i].is_parent()) {
                            tree(path,sock,root.back());
                        }
                        path.pop_back();
                    }
                }
            }

        }

    }
}

