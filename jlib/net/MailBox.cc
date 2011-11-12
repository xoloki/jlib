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

#include <jlib/net/MailBox.hh>

namespace jlib {
	namespace net {

        MailNode::MailNode(std::string name, bool folder, bool parent) {
            m_path.push_back(name);
            m_is_folder=folder;
            m_is_parent=parent;
            m_folder = 0;
        }

        MailNode::MailNode(std::list<std::string> path, bool folder, bool parent) {
            m_path = path;
            m_is_folder=folder;
            m_is_parent=parent;
            m_folder = 0;
        }

        MailNode& MailNode::operator[](unsigned int i) { return m_children[i]; }

        MailNode::iterator MailNode::find(std::list<std::string> path) {
            if(path.size() == 0) return end();
            std::string next = path.front();
            for(iterator i=begin();i!=end();i++) {
                if(i->get_name() == next) {
                    if(path.size() == 1) {
                        return i;
                    }
                    else {
                        std::list<std::string> tmp = path;
                        tmp.pop_front();
                        return i->find(tmp);
                    }
                }
            }
            return end();
        }

        MailNode::const_iterator MailNode::find(std::list<std::string> path) const {
            std::string next = path.front();
            for(const_iterator i=begin();i!=end();i++) {
                if(i->get_name() == next) {
                    if(path.size() == 1) {
                        return i;
                    }
                    else {
                        //std::list<std::string> tmp = path;
                        path.pop_front();
                        return i->find(path);
                    }
                }
            }
            return end();
        }

        std::string MailNode::get_name() const { 
            if(m_path.size() > 0) {
                return m_path.back();
            }
            else {
                return "";
            }
        }

        void MailNode::set_name(std::string n) { 
            if(m_path.size() > 0) {
                m_path.back() = n;
            }
            else {
                m_path.push_back(n);
            }
        }
        
        std::string MailNode::get_delim() const { 
            return m_delim; 
        }

        void MailNode::set_delim(std::string n) { 
            m_delim=n; 
        }
        
        std::list<std::string> MailNode::get_path() const { return m_path; }
        void MailNode::set_path(const std::list<std::string>& n) { m_path=n; }
        
        MailFolder* MailNode::get_folder() const { return m_folder; }
        void MailNode::set_folder(MailFolder* folder) { m_folder = folder; }
        

        std::string MailNode::pathstr(std::list<std::string> path, 
                                      std::string delim, 
                                      bool begin_delim,
                                      bool end_delim,
                                      bool only_delim) {
            std::string ret;

            std::list<std::string>::iterator i = path.begin();
            std::list<std::string>::iterator j;
            for(;i != path.end();i++) {
                j = i; j++;

                if(begin_delim && i == path.begin()) {
                    ret += delim;
                }

                ret += *i;
                if(j != path.end()) {
                    ret += delim;
                }
                
                if(end_delim && j == path.end()) {
                    ret += delim;
                }
            }
            
            if(only_delim && path.size() == 0) {
                ret += delim;
            }

            return ret;
        }
        
        bool operator<(const MailNode& g1, const MailNode& g2) {
            return (g1.get_name() < g2.get_name());
        }
        

        MailBox::MailBox(BoxBuf* buf) {
            m_buf  = buf;
        }

        MailBox::MailBox(std::string name, BoxBuf* buf) {
            m_name = name;
            m_buf  = buf;
        }

        MailBox::~MailBox() {
            
        }

        void MailBox::init() {
            m_buf->clear();
            m_buf->list();
        }

        std::list<std::string> MailBox::get_folders(std::list<std::string> path) {
            std::list<std::string> ret;

            MailNode::iterator i = m_buf->find(path);
            if(i != m_buf->end()) {
                for(MailNode::iterator j = i->begin(); j!=i->end(); j++) {
                    if(j->is_folder()) {
                        ret.push_back(j->get_name());
                    }
                }
            }

            return ret;
        }

        std::list<std::string> MailBox::get_parents(std::list<std::string> path) {
            std::list<std::string> ret;

            MailNode::iterator i = m_buf->find(path);
            if(i != m_buf->end()) {
                for(MailNode::iterator j = i->begin(); j!=i->end(); j++) {
                    if(j->is_parent()) {
                        ret.push_back(j->get_name());
                    }
                }
            }

            return ret;
        }
         
        std::list<std::string> MailBox::get_all(std::list<std::string> path) {
            std::list<std::string> ret;

            MailNode::iterator i = m_buf->find(path);
            if(i != m_buf->end()) {
                for(MailNode::iterator j = i->begin(); j!=i->end(); j++) {
                    ret.push_back(j->get_name());
                }
            }

            return ret;
        }


        bool MailBox::is_folder(std::list<std::string> path) {
            MailNode::iterator i = m_buf->find(path);
            return (i != m_buf->end() && i->is_folder());
        }

        bool MailBox::is_parent(std::list<std::string> path) {
            MailNode::iterator i = m_buf->find(path);
            return (i != m_buf->end() && i->is_parent());
        }


        void MailBox::create_folder(std::list<std::string> path) {
            m_buf->create_folder(path);
        }

        void MailBox::delete_folder(std::list<std::string> path) {
            m_buf->delete_folder(path);
        }

        void MailBox::rename_folder(std::list<std::string> path, std::list<std::string> npath) {
            m_buf->rename_folder(path,npath);
        }
        
        MailNode& MailBox::get_root() {
            return m_buf->get_root();
        }

        MailNode& MailBox::get_root(std::list<std::string> path) {
            MailNode::iterator i = m_buf->find(path);
            return *i;
        }

        const MailNode& MailBox::get_root() const {
            return m_buf->get_root();
        }

        const MailNode& MailBox::get_root(std::list<std::string> path) const {
            MailNode::const_iterator i = m_buf->find(path);
            return *i;
        }

        std::vector<MailNode> MailBox::get_tree(std::list<std::string> path) {
            std::vector<MailNode> ret;

            MailNode::iterator i = m_buf->find(path);
            if(i != m_buf->end()) {
                for(MailNode::iterator j = i->begin(); j != i->end(); j++) {
                    ret.push_back(*j);
                }
            }

            return ret;
        }
        
        MailFolder* MailBox::get_folder(std::list<std::string> path) {
            m_buf->fill(path);
            MailNode::iterator i = m_buf->find(path);
            if(i != m_buf->end()) {
                return i->get_folder();
            }
            else {
                return 0;
            }
        }
        
    }

}
