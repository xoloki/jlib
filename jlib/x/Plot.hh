/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
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


#include <jlib/math/math.hh>
#include <jlib/math/Plot.hh>
#include <jlib/x/Window.hh>

#include <vector>
#include <stack>


namespace jlib {
namespace x {

        
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
      Window("jlib Plot", w, h)
{    
    set_auto_flush(false);

    configure_notify.connect([this](auto&&... a) { return this->on_configure(a...); });
}


template<typename T>
inline
void Plot<T>::draw_point(std::pair<uint,uint> p, uint) {
    Window::draw_point(p);
}


template<typename T>
inline
void Plot<T>::draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2,
                        uint, uint) {
    Window::draw_line(p1, p2);
}


template<typename T>
void
inline Plot<T>::draw() {
    clear();
    set_mode(Window::DRAW);

    math::Plot<T>::draw();

    flush();
}

template<typename T>
void
inline Plot<T>::on_configure(int width, int height) {
    this->width = width;
    this->height = height;

    draw();
}


}
}
