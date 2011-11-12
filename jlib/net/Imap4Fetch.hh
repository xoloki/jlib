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

#ifndef JLIB_NET_IMAP4FETCH_HH
#define JLIB_NET_IMAP4FETCH_HH

#include <jlib/net/Imap4.hh>
#include <jlib/net/MailFetch.hh>

namespace jlib {
    namespace net {

        class Imap4Fetch : public MailFetch, public Imap4 {
        public:
            /**
             * Create Imap4 with given username, password, and host
             *
             */
            Imap4Fetch(jlib::util::URL url, bool remove=false);
            
            /**
             * Destructor.
             */
            virtual ~Imap4Fetch();
            
            // MailFetch virtuals

            virtual std::vector<Email> retrieve();
        };
        
    }
}
#endif //JLIB_NET_IMAP4_HH
