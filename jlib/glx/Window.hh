/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 2008 Joe Yandle <jwy@divisionbyzero.com>
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

#ifndef JLIB_GLX_WINDOW_HH
#define JLIB_GLX_WINDOW_HH

#include <jlib/x/Window.hh>
#include <GL/glx.h>

namespace jlib {
namespace glx {
        
class Window : public x::Window {
public:
    Window(std::string title="jlib::glx::Window", int w=400, int h=400, bool depth = true);
    virtual ~Window();
    
    virtual void clear();
    virtual void flush();
    virtual void iterate();
    
    //Window& operator<<(std::string msg);
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
