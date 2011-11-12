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

#ifndef JLIB_NET_IMAP4BOX_HH
#define JLIB_NET_IMAP4BOX_HH

#include <string>
#include <iostream>
#include <map>
#include <vector>

#include <jlib/net/MailBox.hh>
#include <jlib/net/Imap4.hh>

namespace jlib {
	namespace net {
        
        class Imap4BoxBuf : public Imap4, public BoxBuf {
        public:
            Imap4BoxBuf(jlib::util::URL url);

            virtual void list();

            virtual void fill(std::list<std::string> path);

            virtual void create_folder(std::list<std::string> path);
            virtual void delete_folder(std::list<std::string> path);
            virtual void rename_folder(std::list<std::string> path, std::list<std::string> npath);

        protected:
            void tree(std::list<std::string> path, jlib::sys::socketstream& sock, reference root);
        };

        class Imap4Box : public MailBox {
        public:
            Imap4Box(jlib::util::URL url)
                : MailBox(0)
            {
                m_buf = new Imap4BoxBuf(url);
            }
        };
        
    }
}

#endif //JLIB_NET_MAILGROUP_HH
