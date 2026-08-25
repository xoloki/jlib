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

#include <jlib/net/MailFolder.hh>

#include <algorithm>

namespace jlib {
    namespace net {
        
        MailFolder::MailFolder(FolderBuffer* buffer) {
            m_rep.reset(buffer);
        }
        
        MailFolder::~MailFolder() {
        }
        
        jlib::util::Headers MailFolder::get_headers(unsigned int i) {
            return m_rep->at(i).headers();
        }
        

        unsigned int MailFolder::get_size(unsigned int i) {
            return m_rep->at(i).get_data_size();
        }

        std::set<Email::flag_type> MailFolder::get_flags(unsigned int i) {
            return m_rep->at(i).get_flags();
        }

        Email& MailFolder::get(unsigned int i) {
            if(!m_rep->filled(i)) {
                std::list<unsigned int> j;
                j.push_back(i);
                m_rep->fill(j);
            }
            return m_rep->at(i);
        }
        
        void MailFolder::add(std::vector<Email>& e) {
            m_rep->add(e);
        }
        
        void MailFolder::remove(std::list<unsigned int> which) {
            std::set<Email::flag_type> flags;
            flags.insert(Email::deleted_flag);
            m_rep->set_flags(flags,which);
            for(std::list<unsigned int>::iterator i=which.begin();i!=which.end();i++) {
                m_rep->at(*i).set_flags(flags);
            }
        }
        
        
        void MailFolder::unremove(std::list<unsigned int> which) {
            std::set<Email::flag_type> flags;
            flags.insert(Email::deleted_flag);
            m_rep->unset_flags(flags,which);
            for(std::list<unsigned int>::iterator i=which.begin();i!=which.end();i++) {
                m_rep->at(*i).unset_flags(flags);
            }
        }

        void MailFolder::set_answered(std::list<unsigned int> which) {
            std::set<Email::flag_type> flags;
            flags.insert(Email::answered_flag);
            m_rep->set_flags(flags,which);
            for(std::list<unsigned int>::iterator i=which.begin();i!=which.end();i++) {
                m_rep->at(*i).set_flags(flags);
            }
        }

        void MailFolder::set_unanswered(std::list<unsigned int> which) {
            std::set<Email::flag_type> flags;
            flags.insert(Email::answered_flag);
            m_rep->unset_flags(flags,which);
            for(std::list<unsigned int>::iterator i=which.begin();i!=which.end();i++) {
                m_rep->at(*i).unset_flags(flags);
            }
        }


        
        bool MailFolder::modified() {
            return m_rep->modified();
        }
        
        void MailFolder::refresh() {
            m_rep->scan(true);
        }
        
        void MailFolder::scan() {
            m_rep->scan();
        }
        
        void MailFolder::sort() {
            std::sort(begin(),end());
        }
        
        void MailFolder::filter() {
            
        }
        
        void MailFolder::expunge() {
            m_rep->sync();
        }
        
    }
}
