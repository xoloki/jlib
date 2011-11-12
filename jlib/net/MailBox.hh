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

#ifndef JLIB_NET_MAILBOX_HH
#define JLIB_NET_MAILBOX_HH

#include <string>
#include <iostream>
#include <map>
#include <vector>
#include <list>

#include <sigc++/sigc++.h>

#include <jlib/net/MailFolder.hh>

namespace jlib {
	namespace net {

        /**
         * Class MailNode represents the root node of a directory 
         * tree.  Each node has a name and some children, and also
         * knows what kind of node it is.  A node can be a folder node
         * and/or a parent node.  It is explicitly allowed to be both.
         */
        class MailNode {
        public:

            typedef std::vector<MailNode> rep_type;

            typedef rep_type::pointer pointer;
            typedef rep_type::const_pointer const_pointer;
            typedef rep_type::reference reference;
            typedef rep_type::const_reference const_reference;
            typedef rep_type::iterator iterator;
            typedef rep_type::const_iterator const_iterator; 
            typedef rep_type::reverse_iterator reverse_iterator;
            typedef rep_type::const_reverse_iterator const_reverse_iterator;
            typedef rep_type::size_type size_type;
            typedef rep_type::difference_type difference_type;
            typedef rep_type::allocator_type allocator_type;            


            MailNode(std::string name="INBOX", bool folder=true, bool parent=false);
            MailNode(std::list<std::string> path, bool folder, bool parent);

            MailNode& operator[](unsigned int i);
            friend bool operator<(const MailNode& g1, const MailNode& g2);
            
            iterator find(std::list<std::string> path);
            const_iterator find(std::list<std::string> path) const;

            void push_back(const MailNode& g) { m_children.push_back(g); }
            void push_front(const MailNode& g) { m_children.insert(begin(),g); }

            bool is_folder() const { return m_is_folder; }
            bool is_parent() const { return m_is_parent; }
            bool is_filled() const { return (m_folder != 0); }
            
            std::string get_name() const;
            void set_name(std::string n);

            std::string get_delim() const;
            void set_delim(std::string n);

            std::list<std::string> get_path() const;
            void set_path(const std::list<std::string>& n);

            MailFolder* get_folder() const;
            void set_folder(MailFolder* folder);

            reference front() { return m_children.front(); }
            const_reference front() const { return m_children.front(); }

            reference back() { return m_children.back(); }
            const_reference back() const { return m_children.back(); }

            iterator begin() { return m_children.begin(); }
            const_iterator begin() const { return m_children.begin(); }
            iterator end() { return m_children.end(); }
            const_iterator end() const { return m_children.end(); }
            reverse_iterator rbegin() { return m_children.rbegin(); }
            const_reverse_iterator rbegin() const { return m_children.rbegin(); }
            reverse_iterator rend() { return m_children.rend(); }
            const_reverse_iterator rend() const { return m_children.rend(); }
            bool empty() const { return m_children.empty(); }
            size_type size() const { return m_children.size(); }
            void clear() { m_children.clear(); }

            static std::string pathstr(std::list<std::string> path, 
                                       std::string delim="/", 
                                       bool begin_delim = true, 
                                       bool end_delim = false,
                                       bool only_delim = false);
            
        protected:
            std::string m_delim;
            std::list<std::string> m_path;
            std::vector<MailNode> m_children;
            bool m_is_folder;
            bool m_is_parent;
            MailFolder* m_folder;
        };
        

        /**
         * BoxBuf abstracts the differences between e.g. Imap, mbox, and maildor mailboxes
         */
        class BoxBuf {
        public:
            typedef MailNode::rep_type rep_type;

            typedef rep_type::pointer pointer;
            typedef rep_type::const_pointer const_pointer;
            typedef rep_type::reference reference;
            typedef rep_type::const_reference const_reference;
            typedef rep_type::iterator iterator;
            typedef rep_type::const_iterator const_iterator; 
            typedef rep_type::reverse_iterator reverse_iterator;
            typedef rep_type::const_reverse_iterator const_reverse_iterator;
            typedef rep_type::size_type size_type;
            typedef rep_type::difference_type difference_type;
            typedef rep_type::allocator_type allocator_type;            

            virtual ~BoxBuf() {}
            /**
             * list means to get the path information in the tree, but don't
             * worry about creating MailFolders, i.e.
             */
            virtual void list() = 0;
            /**
             * fill means to not only have path information in the tree, but
             * also to have the specific MailFolder created at that node, i.e.
             * MailNode::is_filled() == true.
             */
            virtual void fill(std::list<std::string> path) = 0;

            virtual void create_folder(std::list<std::string> path) = 0;
            virtual void delete_folder(std::list<std::string> path) = 0;
            virtual void rename_folder(std::list<std::string> path, std::list<std::string> npath) = 0;

            iterator find(std::list<std::string> path) { return m_root.find(path); }
            const_iterator find(std::list<std::string> path) const { return m_root.find(path); }

            iterator end() { return m_root.end(); }
            const_iterator end() const { return m_root.end(); }
            
            MailNode& get_root() { return m_root; }
            const MailNode& get_root() const { return m_root; }

            void clear() { m_root.clear(); }
        protected:
            MailNode m_root;
        };

        /**
         * class MailBox is a group of logically connected MailFolder objects.
         * 
         * 
         */
        class MailBox : public sigc::trackable {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = std::string("jlib::net::MailBox exception")+( (msg=="")?"":": ")+msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };


            MailBox(BoxBuf* buf);
            MailBox(std::string name, BoxBuf* buf);
            virtual ~MailBox();

            void init();

            std::list<std::string> get_folders(std::list<std::string> path);
            std::list<std::string> get_parents(std::list<std::string> path);
            std::list<std::string> get_all(std::list<std::string> path);

            bool is_folder(std::list<std::string> path);
            bool is_parent(std::list<std::string> path);

            void create_folder(std::list<std::string> path);
            void delete_folder(std::list<std::string> path);
            void rename_folder(std::list<std::string> path, std::list<std::string> npath);
            
            MailNode& get_root();
            MailNode& get_root(std::list<std::string> path);

            const MailNode& get_root() const;
            const MailNode& get_root(std::list<std::string> path) const;

            std::vector<MailNode> get_tree(std::list<std::string> path);

            MailFolder* get_folder(std::list<std::string> path);

            void set_name(std::string n) { m_name = n; }
            std::string get_name() { return m_name; }
            
        protected:
            BoxBuf* m_buf;
            std::string m_name;
        };
        
    }
}

#endif //JLIB_NET_MAILGROUP_HH
