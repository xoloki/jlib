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

#include <jlib/x/Display.hh>

#include <iostream>
#include <sstream>

namespace jlib {
namespace x {
        
	
Display::Display()
{
    m_dpy = XOpenDisplay(NULL);
}
    
Display::~Display() {}
    
void Display::send_event(XEvent e) {
    Status s = XSendEvent(m_dpy, InputFocus, True, 0, &e);
    XFlush(m_dpy);
    //std::cout << "Display::send_event: status = " << s << std::endl;
}

KeySym Display::sym(const std::string& s) const {
    return XStringToKeysym(s.data());
}

KeyCode Display::code(KeySym sym) const {
    return XKeysymToKeycode(m_dpy, sym);
}

KeyCode Display::code(const std::string& s) const {
    return XKeysymToKeycode(m_dpy, sym(s));
}
	
}
}
