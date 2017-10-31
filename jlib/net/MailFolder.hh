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

#ifndef JLIB_NET_MAILFOLDER_HH
#define JLIB_NET_MAILFOLDER_HH

#include <jlib/net/Email.hh>

#include <sigc++/trackable.h>

#include <string>
#include <iostream>
#include <map>
#include <set>
#include <vector>
#include <list>

namespace jlib {
	namespace net {

        class FolderBuffer : public sigc::trackable {
        public:
            
            typedef std::vector<Email> rep_type;

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

            virtual bool modified() = 0;
            virtual void scan(bool check_modified=false) = 0;

            virtual void set_flags(std::set<Email::flag_type> flags, std::list<unsigned int> which)=0;
            virtual void unset_flags(std::set<Email::flag_type> flags, std::list<unsigned int> which)=0;
            virtual void sync() = 0;
            virtual void fill(std::list<unsigned int> which) = 0;

            virtual void add(std::vector<Email> mails) = 0;

            iterator begin() { return m_rep.begin(); }
            const_iterator begin() const { return m_rep.begin(); }
            iterator end() { return m_rep.end(); }
            const_iterator end() const { return m_rep.end(); }
            reverse_iterator rbegin() { return m_rep.rbegin(); }
            const_reverse_iterator rbegin() const { return m_rep.rbegin(); }
            reverse_iterator rend() { return m_rep.rend(); }
            const_reverse_iterator rend() const { return m_rep.rend(); }
            bool empty() const { return m_rep.empty(); }
            size_type size() const { return m_rep.size(); }

            reference at(unsigned int i) { return m_rep[i]; }
            const_reference at(unsigned int i) const { return m_rep[i]; }

            bool filled(unsigned int i) const { return m_filled[i]; }
        protected:
            std::vector<Email> m_rep;
            std::vector<bool> m_filled;

        };
        
        class MailFolder : public sigc::trackable {
        public:
            typedef std::vector<Email> rep_type;

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

            MailFolder(FolderBuffer* buffer);
            virtual ~MailFolder();


            virtual Email& get(unsigned int i);

            virtual void add(std::vector<Email>& e);

            virtual void remove(std::list<unsigned int> which);
            virtual void unremove(std::list<unsigned int> which);

            virtual void set_answered(std::list<unsigned int> which);
            virtual void set_unanswered(std::list<unsigned int> which);

            virtual bool modified();

            virtual void refresh();
            virtual void scan();

            virtual void sort();
            virtual void filter();

            virtual void expunge();

            reference at(unsigned int i) { return m_rep->at(i); }
            const_reference at(unsigned int i) const { return m_rep->at(i); }

            jlib::util::Headers get_headers(unsigned int i);
            unsigned int get_size(unsigned int i);
            std::set<Email::flag_type> get_flags(unsigned int i);

            void init(FolderBuffer* buffer) { m_rep = buffer; }

            Email copy(unsigned int i) { return get(i); }

            void sort_field(std::string field) { m_sort_field = field; }
            std::string sort_field() { return m_sort_field; }

            void filter_rules(std::multimap<std::string,std::string>& rules) { m_filter_rules = rules; }
            std::multimap<std::string,std::string>& filter_rules() { return m_filter_rules; }

            void autosort(bool srt) { m_autosort = srt; }
            bool autosort() { return m_autosort; }

            iterator begin() { return m_rep->begin(); }
            const_iterator begin() const { return m_rep->begin(); }
            iterator end() { return m_rep->end(); }
            const_iterator end() const { return m_rep->end(); }
            reverse_iterator rbegin() { return m_rep->rbegin(); }
            const_reverse_iterator rbegin() const { return m_rep->rbegin(); }
            reverse_iterator rend() { return m_rep->rend(); }
            const_reverse_iterator rend() const { return m_rep->rend(); }
            bool empty() const { return m_rep->empty(); }
            size_type size() const { return m_rep->size(); }

        protected:
            FolderBuffer* m_rep;

            /**
             * field to sort by
             */
            std::string m_sort_field;

            /**
             * should we sort every time we scan?
             */
            bool m_autosort;

            /**
             * the key-val pairs that define mail filtering rules
             */
            std::multimap<std::string,std::string> m_filter_rules;
        };
        
    }
}

#endif //JLIB_NET_MAILFOLDER_HH
