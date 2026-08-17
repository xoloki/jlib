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

#include <jlib/glfw/Plot.hh>
#include "Hyper.hh"
#include <iostream>
#include <unistd.h>

typedef double T;
typedef jlib::glfw::Plot<T> PlotType;

class GLFWPlot : public HyperPlot<T, PlotType> {
public:
    GLFWPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
        : HyperPlot<T, PlotType>(n, c, w, h)
    {
        // Per-object signals, the same surface jhyper binds on x::Window.
        // glut::Main could only publish process-global statics, because
        // glutMainLoop() never returned and owned the dispatch itself.
        key_press.connect([this](auto&&... a) { return this->key_pressed(a...); });
        button_press.connect([this](auto&&... a) { return this->button_pressed(a...); });
        timeout.connect([this](auto&&... a) { return this->on_timeout(a...); });
    }

    // GLFW reports typed text, as X does; HyperPlot wants a single char.
    void key_pressed(const std::string& key, int x, int y) {
        if(key.empty())
            return;

        HyperPlot<T, PlotType>::key_pressed(key[0], x, y);
    }

    // x::Window and glfw::Window report (button, x, y); HyperPlot expects a
    // state argument in the middle, as GLUT supplied.
    void button_pressed(int button, int x, int y) {
        HyperPlot<T, PlotType>::button_pressed(button, 0, x, y);
    }

    void set_color(const triple<T>& color) {
        GLfloat fcolors[4];
        fcolors[0] = color.r; fcolors[1] = color.g; fcolors[2] = color.b; fcolors[3] = 0.5;
        glColor3fv(fcolors);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, fcolors);
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
        GLFWPlot plot(D, std::vector< std::pair<T,T> >(), 700, 700);

        plot.run();
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

