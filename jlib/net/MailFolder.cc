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

#include <jlib/net/MailFolder.hh>

#include <algorithm>

namespace jlib {
    namespace net {
        
        MailFolder::MailFolder(FolderBuffer* buffer) {
            m_rep = buffer;
        }
        
        MailFolder::~MailFolder() {
            delete m_rep;
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
