/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2008 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_GLX_WINDOW_HH
#define JLIB_GLX_WINDOW_HH

#include <jlib/x/Window.hh>
#include <GL/glx.h>

namespace jlib {
namespace glx {
        
class Window : public x::Window {
public:
    Window(const std::string& title="jlib::glx::Window", int w=400, int h=400, bool depth = true);
    virtual ~Window();
    
    virtual void clear();
    virtual void flush();
    virtual void iterate();
    
    //Window& operator<<(const std::string& msg);
    //Window& operator<<(int value);

    void on_configure(int w, int h);
    
protected:
    void init_glx();

    //GLXWindow m_glxwin;
    GLXContext m_glxctx;
    XVisualInfo* m_vinfo;
    bool m_double;
    bool m_depth;
};
    
}
}

#endif
