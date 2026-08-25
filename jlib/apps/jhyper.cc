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
