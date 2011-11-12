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

#include <jlib/glx/Window.hh>
#include <jlib/glu/projection.hh>
#include <GL/glu.h>
#include <GL/gl.h>

namespace jlib {
namespace glx {

static int single_buffer[] = { GLX_RGBA,
                               GLX_DEPTH_SIZE, 16,
                               None };

static int double_buffer[] = { GLX_RGBA,
                               GLX_DOUBLEBUFFER,
                               GLX_DEPTH_SIZE, 16,
                               None };

Window::Window(std::string title, int w, int h, bool depth) 
    : x::Window(),
      m_depth(depth)
{	  
    bool swap = true;
    XEvent evt;
    bool expose = false;

    m_width = w;
    m_height = h;
    m_auto_flush = true;
    
    m_dpy = XOpenDisplay(NULL);
    m_screen = DefaultScreen(m_dpy);

    m_vinfo = glXChooseVisual(m_dpy, m_screen, double_buffer);

    if(!m_vinfo) {
        m_vinfo = glXChooseVisual(m_dpy, m_screen, single_buffer);
        m_double = false;
    } else {
        m_double = true;
    }

    m_visual = m_vinfo->visual;
    m_glxctx = glXCreateContext(m_dpy, m_vinfo, 0, GL_TRUE);

    m_swa.border_pixel = BlackPixel(m_dpy, m_screen);
    m_swa.colormap = XCreateColormap(m_dpy, RootWindow(m_dpy, m_vinfo->screen), m_visual, AllocNone);
    unsigned long values = (CWBorderPixel|CWColormap);
    
    m_win = XCreateWindow(m_dpy, 
                          RootWindow(m_dpy, m_vinfo->screen), 
                          0, 0, 
                          w, h, 
                          0, 
                          m_vinfo->depth, 
                          InputOutput, 
                          m_visual,
                          values,
                          &m_swa);
    
    select_input(ExposureMask | 
                 StructureNotifyMask | 
                 KeyPressMask | 
                 ButtonPressMask | 
                 ButtonReleaseMask);
    
    set_title(title);

    XMapWindow(m_dpy, m_win);
    while(!expose) {
		XNextEvent(m_dpy, &evt);
		if(evt.type == Expose) expose = true;
    }

    init_glx();

    clear();
    flush();
}
    
Window::~Window() {
    
}
    
void Window::init_glx() {
    glXMakeCurrent(m_dpy, m_win, m_glxctx);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    on_configure(m_width, m_height);
}

void Window::on_configure(int w, int h) {
    if(m_depth)
        glu::projection::perspective(w, h);
    else
        glu::projection::ortho2d(w, h);
}
	
void Window::clear() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Window::flush() { 
    if(m_double)
        glXSwapBuffers(m_dpy, m_win);
    else
        glFlush();
}
	
	
void Window::iterate() {
    x::Window::iterate();
}

	
}
}
