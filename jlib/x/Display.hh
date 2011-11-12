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

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <string>

namespace jlib {
namespace x {
        
class Display {
public:
    Display();
    virtual ~Display();
    
    void send_event(XEvent e);

    KeySym sym(std::string s) const;

    KeyCode code(KeySym sym) const;
    KeyCode code(std::string s) const;
    
protected:
    ::Display* m_dpy;
};
    
}
}
