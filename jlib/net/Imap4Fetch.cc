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
#include <jlib/net/Imap4Fetch.hh>

#include <jlib/sys/sys.hh>

#include <jlib/util/util.hh>

#include <sstream>
#include <memory>

namespace jlib {
    namespace net {
        
        Imap4Fetch::Imap4Fetch(jlib::util::URL url, bool remove) 
            : MailFetch(url,remove),
              Imap4(url)
        {
            
        }

        Imap4Fetch::~Imap4Fetch() {
            
        }
        
        std::vector<Email> Imap4Fetch::retrieve() {
            std::vector<Email> ret;
            std::auto_ptr<jlib::sys::socketstream> sock(connect());
            login(*sock);
            select(*sock,"INBOX");

            for(unsigned int i=1; i<=m_exists; i++) {
                std::string data = this->Imap4::retrieve(i);
                ret.push_back(Email(data));
            }

            logout(*sock);
            disconnect(*sock);

            return ret;
        }


    }
}
