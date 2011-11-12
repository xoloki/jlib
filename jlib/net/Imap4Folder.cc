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

#include <jlib/net/net.hh>
#include <jlib/net/Imap4Folder.hh>

#include <jlib/util/util.hh>
#include <jlib/util/URL.hh>

#include <sstream>
#include <algorithm>
#include <memory>

namespace jlib {
    namespace net {


        Imap4FolderBuffer::Imap4FolderBuffer(jlib::util::URL url) 
            : Imap4(url)
        {
            if(url.get_path() != "" && url.get_path() != "/") {
                if(url.get_delim() != "") {
                    std::string delim = url.get_delim();
                    m_path = MailNode::pathstr(jlib::util::tokenize_list(url.get_path()),delim,false);
                }
                else {
                    m_path = url.get_path().substr(url.get_path().find_first_not_of("/"));
                }
                
                //
            }
            else {
                m_path = "inbox";
            }
            m_scanned = false;            
        }
        
        Imap4FolderBuffer::~Imap4FolderBuffer() {}
        
        bool Imap4FolderBuffer::modified() {
            if(!m_scanned) return true;
            unsigned int exists = this->exists();
            unsigned int recent = this->recent();
            
            std::auto_ptr<jlib::sys::socketstream> sock(connect());
            login(*sock);
            
            examine(*sock,m_path);
            
            logout(*sock);
            disconnect(*sock);

            return ( this->exists() != exists || this->recent() > recent );
        }

        void Imap4FolderBuffer::scan(bool check_modified) {
            unsigned int exists = this->exists();
            unsigned int recent = this->recent();

            std::auto_ptr<jlib::sys::socketstream> sock(connect());
            //sock = std::auto_ptr(connect());
            login(*sock);
            examine(*sock,m_path);

            if(check_modified) {
                if(m_scanned) {
                    if(this->exists() == exists && this->recent() <= recent) {
                        logout(*sock);
                        disconnect(*sock);
                        return;
                    }
                }
            }
            
            m_rep.clear();
            m_filled.clear();
            
            //cout << "for(i=0;i<"<<m_exists<<";i++)" <<endl;
            for(unsigned int i=0;i<m_exists;i++) {
                unsigned int size;
                //cout << "retrieve_headers["<<i<<"]" <<endl;
                std::string h = Imap4::retrieve_headers(*sock,i,m_path,size);
                //cout << "headers["<<i<<"] = '"<<h<<"'"<<endl;
                //std::istringstream is(h);
                std::multimap<std::string,std::string> head;
                Email email(h);
                //jlib::net::parse_headers(is,head);
                //m_headers.push_back(head);
                //m_logical.push_back(i);
                email.set_data_size(size);

                std::set<Email::flag_type> flags;
                std::vector<std::string> f; f.push_back("FLAGS");
                std::string line = fetch(*sock,std::make_pair(i+1,i+1),f)[0];
                std::string s = "FLAGS ";
                line = line.substr(line.find(s)+s.length());
                line = jlib::util::slice(line,"(",")");
                std::vector<std::string> flg = jlib::util::tokenize(line);
                
                std::vector<std::string> upper;
                std::transform(flg.begin(),flg.end(),back_inserter(upper),jlib::util::upper);

                if(find(upper.begin(),upper.end(),"\\SEEN") != upper.end()) {
                    flags.insert(Email::seen_flag);
                }

                if(find(upper.begin(),upper.end(),"\\DELETED") != upper.end()) {
                    flags.insert(Email::deleted_flag);
                }

                if(find(upper.begin(),upper.end(),"\\ANSWERED") != upper.end()) {
                    flags.insert(Email::answered_flag);
                }
                
                email.clear_flags();
                email.set_flags(flags);

                m_rep.push_back(email);
                m_filled.push_back(false);
            }
            m_scanned = true;
            
            logout(*sock);
            disconnect(*sock);
        }
        
        void Imap4FolderBuffer::set_flags(std::set<Email::flag_type> flags, std::list<unsigned int> which) {
            std::string key = "+FLAGS.SILENT";
            std::vector<std::string> vals;
            std::set<Email::flag_type>::iterator j = flags.begin();
            for(;j!=flags.end();j++) {
                switch(*j) {
                case Email::answered_flag:
                    vals.push_back("\\Answered");
                    break;
                case Email::deleted_flag:
                    vals.push_back("\\Deleted");
                    break;
                case Email::seen_flag:
                    vals.push_back("\\Seen");
                    break;
                }
            }
            
            std::auto_ptr<jlib::sys::socketstream> sock(connect());
            //sock(connect())
            login(*sock);
            select(*sock,m_path);
            for(std::list<u_int>::iterator i=which.begin();i!=which.end();i++) {
                store(*sock,std::make_pair(*i+1,*i+1),key,vals);
            }            

            logout(*sock);
            disconnect(*sock);
        }

        void Imap4FolderBuffer::unset_flags(std::set<Email::flag_type> flags, std::list<unsigned int> which) {
            std::string key = "-FLAGS.SILENT";
            std::vector<std::string> vals;
            std::set<Email::flag_type>::iterator j = flags.begin();
            for(;j!=flags.end();j++) {
                switch(*j) {
                case Email::answered_flag:
                    vals.push_back("\\Answered");
                    break;
                case Email::deleted_flag:
                    vals.push_back("\\Deleted");
                    break;
                case Email::seen_flag:
                    vals.push_back("\\Seen");
                    break;
                }
            }

            std::auto_ptr<jlib::sys::socketstream> sock(connect());
            login(*sock);
            select(*sock,m_path);
            
            for(std::list<u_int>::iterator i=which.begin();i!=which.end();i++) {
                store(*sock,std::make_pair(*i+1,*i+1),key,vals);
            }            

            logout(*sock);
            disconnect(*sock);
        }

        void Imap4FolderBuffer::sync() {
            std::auto_ptr<jlib::sys::socketstream> sock(connect());
            login(*sock);
            select(*sock,m_path);
            
            Imap4::expunge(*sock);
            
            logout(*sock);
            disconnect(*sock);
        }

        void Imap4FolderBuffer::fill(std::list<unsigned int> which) {
            for(std::list<u_int>::iterator i=which.begin();i!=which.end();i++) {
                unsigned int j = *i;
                if(!filled(j)) {
                    std::string data = Imap4::retrieve(j,m_path);
                    if(data != "") {
                        //std::istringstream is(data);
                        m_rep[j].create(data);
                    }
                    m_filled[j] = true;
                    std::set<Email::flag_type> flags;
                    flags.insert(Email::seen_flag);
                    m_rep[j].set_flags(flags);
                }
            }
        }
        
        void Imap4FolderBuffer::add(std::vector<Email> mails) {
            std::auto_ptr<jlib::sys::socketstream> sock(connect());
            login(*sock);

            for(unsigned int i=0;i<mails.size();i++) {
                append(*sock, m_path, mails[i].raw());
            }
            
            logout(*sock);
            disconnect(*sock);
        }

    }
}

