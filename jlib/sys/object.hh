/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2002 Joe Yandle <jwy@divisionbyzero.com>
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

#ifndef JLIB_SYS_OBJECT_HH
#define JLIB_SYS_OBJECT_HH

#include <sigc++/trackable.h>

namespace jlib {
namespace sys {

class Object : public sigc::trackable { 
public: 
    Object() : m_ref(1) {}
    virtual ~Object() {} 
    
    void reference() { m_ref++; }
    void unreference() { if(--m_ref <= 0) delete this; }

    int refcount() const { return m_ref; }
    
private: 
    int m_ref; 
}; 
             
}
}

#endif //JLIB_SYS_OBJECT_HH
