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


#include <jlib/glx/Plot.hh>
#include <jlib/apps/Hyper.hh>
#include <jlib/util/util.hh>

#include <iostream>
#include <unistd.h>


typedef float T;
typedef jlib::glx::Plot<T> PlotType;


class GLXPlot : public HyperPlot<T, PlotType> {
public:
    GLXPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
        : HyperPlot<T, PlotType>(n, c, w, h)
    {
        key_press.connect([this](auto&&... a) { return this->key_pressed(a...); });
        button_press.connect([this](auto&&... a) { return this->button_pressed(a...); });
        timeout.connect([this](auto&&... a) { return this->on_timeout(a...); });
    }

    void set_color(const triple<T>& color) {
        GLfloat fcolors[4];
        fcolors[0] = color.r; fcolors[1] = color.g; fcolors[2] = color.b; fcolors[3] = 0.5;
        glColor3fv(fcolors);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, fcolors);
    }

    void key_pressed(const std::string& key, int x, int y) {
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

