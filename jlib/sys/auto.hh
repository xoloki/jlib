/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2000 Joe Yandle <jwy@divisionbyzero.com>
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

#ifndef JLIB_SYS_AUTO_HH
#define JLIB_SYS_AUTO_HH

namespace jlib {
    namespace sys {

        template<class T>
        class auto_lock {
        public:
            auto_lock(T& t) : m_t(t) { m_t.lock(); }
            ~auto_lock() { m_t.unlock(); }
        private:
            T& m_t;
        };

    }
}
#endif //JLIB_SYS_AUTO_HH
