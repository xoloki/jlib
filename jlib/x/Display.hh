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

    KeySym sym(const std::string& s) const;

    KeyCode code(KeySym sym) const;
    KeyCode code(const std::string& s) const;
    
protected:
    ::Display* m_dpy;
};
    
}
}
