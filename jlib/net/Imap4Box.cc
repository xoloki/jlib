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
            std::auto_ptr<jlib::sys::socketstream> sock(connect());

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
            std::auto_ptr<jlib::sys::socketstream> sock(connect());
            login(*sock);
            
            create(*sock,m_url.get_path_no_slash()+MailNode::pathstr(path,m_delim,false));

            logout(*sock);
            disconnect(*sock);
        }

        void Imap4BoxBuf::delete_folder(std::list<std::string> path) {
            std::auto_ptr<jlib::sys::socketstream> sock(connect());
            login(*sock);
            
            remove(*sock,m_url.get_path_no_slash()+MailNode::pathstr(path,m_delim,false));

            logout(*sock);
            disconnect(*sock);
        }

        void Imap4BoxBuf::rename_folder(std::list<std::string> path, std::list<std::string> npath) {
            std::auto_ptr<jlib::sys::socketstream> sock(connect());
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

