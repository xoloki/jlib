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

#include <jlib/glx/Window.hh>
#include <jlib/glu/projection.hh>
#include <jlib/gl/opengl.hh>

namespace jlib {
namespace glx {

static int single_buffer[] = { GLX_RGBA,
                               GLX_DEPTH_SIZE, 16,
                               None };

static int double_buffer[] = { GLX_RGBA,
                               GLX_DOUBLEBUFFER,
                               GLX_DEPTH_SIZE, 16,
                               None };

Window::Window(const std::string& title, int w, int h, bool depth) 
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
