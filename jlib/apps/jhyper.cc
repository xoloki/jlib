/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joe Yandle <joey@divisionbyzero.com>
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

#include <iostream>

#include <cstdlib>

#include <unistd.h>

#include <jlib/x/Plot.hh>

#include <jlib/util/util.hh>

#include <jlib/apps/Hyper.hh>

typedef float T;
typedef jlib::x::Plot<T> PlotType;


class XPlot : public HyperPlot<T, PlotType> {
public:
    XPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
        : HyperPlot<T, PlotType>(n, c, w, h),
          m_timeout(10000)
    {
        set_timeout(m_timeout);

        key_press.connect([this](auto&&... a) { return this->key_pressed(a...); });
        button_press.connect([this](auto&&... a) { return this->button_pressed(a...); });
        timeout.connect([this](auto&&... a) { return this->on_timeout(a...); });
    }

    void draw() {
        // Up through HyperPlot rather than straight to the backend.  This
        // used to skip it and keep the colour bookkeeping itself -- zeroing
        // the vertex counter on the way in and clearing the first-frame flag
        // on the way out -- which is exactly the work HyperPlot::draw does
        // now that the primitives carry a vertex index.
        HyperPlot<T, PlotType>::draw();

        this->set_foreground(255, 255, 255);
        this->move(25, 25);
        this->draw_string("N=" + jlib::util::string_value(this->D) + " R=" + util::string_value(this->r) + " T=" + util::string_value((int)this->m_timeout)+"us");
    }

    void set_color(const triple<T>& color) {
        set_foreground(255*color.r, 255*color.g, 255*color.b);
    }
    
    void key_pressed(const std::string& key, int x, int y) {
        HyperPlot<T, PlotType>::key_pressed(key[0], x, y);

        if(key == "y" || key == "h") {
            m_timeout += (key == "y" ? 100 : -100);
            set_timeout(m_timeout);
        }
    }

    void button_pressed(int button, int x, int y) {
        HyperPlot<T, PlotType>::button_pressed(button, 0, x, y);
    }

    void on_timeout() {
        HyperPlot<T, PlotType>::on_timeout();
        this->draw();
    }

    long m_timeout;
};

int main(int argc, char** argv) {
    uint D = 5;
    if(argc > 1) {
        D = atoi(argv[1]);
    }
    
    try {
        XPlot plot(D, std::vector< std::pair<T,T> >(), 700, 700);

        plot.run();
    }
    catch(std::exception& e) {
        std::cerr << "caught std::exception: " << e.what() << std::endl;
        exit(1);
    } catch(...) {
        std::cerr << "caught unknown exception" << std::endl;
        exit(1);
    }

    exit(0);
}
