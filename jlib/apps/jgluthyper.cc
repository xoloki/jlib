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

#include <jlib/glut/Plot.hh>
#include "Hyper.hh"
#include <iostream>
#include <unistd.h>

typedef double T;
typedef jlib::glut::Plot<T> PlotType;

class GLUTPlot : public HyperPlot<T, PlotType> {
public:
    GLUTPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
        : HyperPlot<T, PlotType>(n, c, w, h)
    {
        glut::Main::keyboard.connect(sigc::mem_fun(this, &HyperPlot<T,PlotType>::key_pressed));
        glut::Main::mouse.connect(sigc::mem_fun(this, &HyperPlot<T,PlotType>::button_pressed));
        glut::Main::idle.clear();
        glut::Main::idle.connect(sigc::mem_fun(this, &GLUTPlot::on_timeout));
    }

    void on_timeout() {
        HyperPlot<T, PlotType>::on_timeout();
        
        PlotType::on_idle();
    }
};

int main(int argc, char** argv) {
    uint D = 5;
    if(argc > 1) {
        D = atoi(argv[1]);
    }

    try {
        glut::Main::init(argc, argv, true, true, false);

        GLUTPlot plot(D, std::vector< std::pair<T,T> >(), 700, 700);
        
        glut::Main::run();
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    } catch(...) {
        std::cerr << "unknown exception" << std::endl;
        exit(1);
    }

    exit(0);
}

