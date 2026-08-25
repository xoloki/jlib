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

#ifndef JLIB_NET_MAILFOLDER_HH
#define JLIB_NET_MAILFOLDER_HH

#include <jlib/net/Email.hh>


#include <memory>
#include <utility>
#include <string>
#include <iostream>
#include <map>
#include <set>
#include <vector>
#include <list>

namespace jlib {
	namespace net {

        class FolderBuffer {
        public:
            /**
             * Needed, and it was missing.
             *
             * MailFolder holds its buffer as a shared_ptr<FolderBuffer> and
             * init() takes a FolderBuffer*, so the deleter the shared_ptr
             * stores is typed to this class.  Destroying an MFolder therefore
             * deleted an MFolderBuffer through a base pointer with no virtual
             * destructor -- undefined behaviour, which libc++ turns into a
             * trap, so every mbox folder crashed on the way out.  Declaring
             * ~MFolderBuffer virtual in the derived class did not help: what
             * matters is the static type at the delete.
             *
             * The other four are defaulted because declaring this one
             * suppresses the implicit moves.
             */
            FolderBuffer() = default;
            FolderBuffer(const FolderBuffer&) = default;
            FolderBuffer(FolderBuffer&&) = default;
            FolderBuffer& operator=(const FolderBuffer&) = default;
            FolderBuffer& operator=(FolderBuffer&&) = default;
            virtual ~FolderBuffer() = default;

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
        
        class MailFolder {
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

            /** Takes ownership of the buffer. */
            MailFolder(FolderBuffer* buffer);
            virtual ~MailFolder();

            /**
             * Not copyable, movable.
             *
             * This owned its buffer through a raw pointer and deleted it,
             * with no copy control declared, so copying one copied the
             * pointer and both destructors freed it -- the same shape as the
             * Date and streambuf double frees.  Nothing in the tree copies a
             * MailFolder, and it is a polymorphic base, so a copy would slice
             * as well as double free.  Deleted rather than fixed.
             *
             * Moving is fine and now works: the destructor was suppressing
             * the implicit move operations, so std::move was a copy.
             */
            MailFolder(const MailFolder&) = delete;
            MailFolder& operator=(const MailFolder&) = delete;
            MailFolder(MailFolder&&) = default;
            MailFolder& operator=(MailFolder&&) = default;


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

            void init(FolderBuffer* buffer) { m_rep.reset(buffer); }

            Email copy(unsigned int i) { return get(i); }

            void sort_field(const std::string& field) { m_sort_field = std::move(field); }
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
            // Owned.  A unique_ptr so the ownership is stated rather than
            // implied by a delete in the destructor.
            std::unique_ptr<FolderBuffer> m_rep;

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
