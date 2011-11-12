/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 2008 Joe Yandle <joey@divisionbyzero.com>
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


#include <jlib/glx/Plot.hh>
#include <jlib/apps/Hyper.hh>

#include <iostream>
#include <unistd.h>


typedef double T;
typedef jlib::glx::Plot<T> PlotType;


class GLXPlot : public HyperPlot<T, PlotType> {
public:
    GLXPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
        : HyperPlot<T, PlotType>(n, c, w, h)
    {
        key_press.connect(sigc::mem_fun(this, &GLXPlot::key_pressed));
        button_press.connect(sigc::mem_fun(this, &GLXPlot::button_pressed));
        timeout.connect(sigc::mem_fun(this, &GLXPlot::on_timeout));
    }

    void key_pressed(std::string key, int x, int y) {
        HyperPlot<T, PlotType>::key_pressed(key[0], x, y);
    }

    void button_pressed(int button, int x, int y) {
        HyperPlot<T, PlotType>::button_pressed(button, 0, x, y);
    }

    void on_timeout() {
        HyperPlot<T, PlotType>::on_timeout();
        this->draw();
    }
};

int main(int argc, char** argv) {
    uint D = 5;
    if(argc > 1) {
        D = atoi(argv[1]);
    }
    
    try {
        GLXPlot plot(D, std::vector< std::pair<T,T> >(), 700, 700);

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

