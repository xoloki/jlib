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

#ifndef JLIB_GLX_PLOT_HH
#define JLIB_GLX_PLOT_HH

#include <jlib/math/math.hh>
#include <jlib/math/Plot.hh>
#include <jlib/glx/Window.hh>

#include <vector>
#include <stack>


namespace jlib {
namespace glx {

        
template<typename T>
class Plot : public math::Plot<T>, public Window {
public:
    Plot(uint n, std::vector< std::pair<T,T> > c, uint w=400, uint h=400);

    virtual void draw();
    virtual void draw_point(std::pair<uint,uint> p, uint index);
    virtual void draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2,
                           uint i1, uint i2);

protected:
    void on_configure(int width, int height);
};


template<typename T>
inline
Plot<T>::Plot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
    : math::Plot<T>(n, c, w, h),
      Window("jlib::glx::Plot", w, h, false)
{    
    set_auto_flush(false);

    configure_notify.connect([this](auto&&... a) { return this->on_configure(a...); });
}


template<typename T>
inline
void Plot<T>::draw_point(std::pair<uint,uint> p, uint) {
    glBegin(GL_POINTS);
    glVertex2i(p.first, p.second);
    glEnd();
}


template<typename T>
inline
void Plot<T>::draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2,
                        uint, uint) {
    glBegin(GL_LINES);
    glVertex2i(p1.first, p1.second);
    glVertex2i(p2.first, p2.second);
    glEnd();
}


template<typename T>
void
inline Plot<T>::draw() {
    clear();

    math::Plot<T>::draw();

    flush();
}

template<typename T>
void
inline Plot<T>::on_configure(int width, int height) {
    this->width = width;
    this->height = height;

    glx::Window::on_configure(width, height);

    draw();
}


}
}

#endif
