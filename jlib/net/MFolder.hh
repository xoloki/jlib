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

#ifndef JLIB_NET_MFOLDER_HH
#define JLIB_NET_MFOLDER_HH

#include <jlib/net/MailBox.hh>

namespace jlib {
    namespace net {
        
        class MFolderBuffer : public FolderBuffer {
        public:
            MFolderBuffer(std::string path);

            virtual ~MFolderBuffer();
            
            virtual bool modified();
            virtual void scan(bool check_modified=false);

            virtual void set_flags(std::set<Email::flag_type> flags, std::list<unsigned int> which);
            virtual void unset_flags(std::set<Email::flag_type> flags, std::list<unsigned int> which);
            virtual void sync();
            virtual void fill(std::list<unsigned int> which);

            virtual void add(std::vector<Email> mails);

        protected:
            void scan_divide();
            void scan_headers();
            void remove(std::list<unsigned int> which);

            /**
             * the file this box uses
             */
            std::string m_path;

            /**
             * where the physical boundaries are between the emails
             */
            std::vector<long> m_divide;

            /**
             * when we began the last scan
             */
            time_t m_scan_begin;
        };
        
        class MFolder : public MailFolder {
        public:
            MFolder(std::string path)
                : MailFolder(NULL)
            {
                m_rep = new MFolderBuffer(path);
                init(m_rep);
            }
                
        };
        
    }
}

#endif //NET_MBOX_HH
