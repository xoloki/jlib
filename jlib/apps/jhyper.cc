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

using namespace jlib::math;

const long double PI = 3.14159265358979323846264338;

template<class O>
struct triple {
    O r;
    O g;
    O b;
};

typedef double T;


class HyperPlot : public jlib::x::Plot<T> {
public:
    HyperPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h);

    virtual void change(uint n);
    virtual void draw();
    virtual void draw_point(std::pair<uint,uint> point);
    virtual void draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2);
protected:
    std::vector< triple<int> > colors;
    uint i;
    bool first;
};

uint D = 5;
matrix<T> rotate = matrix<T>::identity(D+1);
matrix<T> back_rotate = matrix<T>::identity(D+1);
bool waiting = false;
std::vector< std::pair<T,T> > clip;
std::vector<T> trans; 

void key_pressed(std::string key,int x,int y, HyperPlot* plot);
void button_pressed(int button,int x,int y);
void on_timeout(HyperPlot* plot);
void initialize_rotation(HyperPlot* plot);

int choose(int m, int k);

int main(int argc, char** argv) {

    clip.push_back(std::make_pair(-1.5, 1.5));
    clip.push_back(std::make_pair(-1.5, 1.5));
    trans.push_back(0); trans.push_back(0); 
    
    if(argc > 1) {
        D = atoi(argv[1]);
    }
    
    for(uint i = 2; i < D; i++) {
        clip.push_back(std::make_pair(1.5, 4.5));
        trans.push_back(3); 
    }
    
    HyperPlot plot(D, clip, 500, 500);
        
    try {
        initialize_rotation(&plot);
        plot * matrix<T>::translate(D, trans);

        cuboid<T> cube(D);
        plot.add(cube);

        plot.set_timeout(10);
        plot.draw();

        plot.key_press.connect(sigc::bind(sigc::ptr_fun(&key_pressed), &plot));
        plot.button_press.connect(sigc::ptr_fun(&button_pressed));
        plot.timeout.connect(sigc::bind(sigc::ptr_fun(&on_timeout), &plot));

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

HyperPlot::HyperPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
    : jlib::x::Plot<T>(n, c, w, h),
      first(true)
{}

void HyperPlot::draw() {
    i = 0;

    jlib::x::Plot<T>::draw();

    this->set_foreground(1, 1, 1);
    this->move(5, 5);
    this->draw_string("N="+jlib::util::string_value(this->D));

    if(first) first = false;
}

void HyperPlot::change(uint n) {
    jlib::x::Plot<T>::change(n);
    first = true;
    colors.clear();
}


void HyperPlot::draw_point(std::pair<uint,uint> point) {
    if(first) {
        triple<int> color; color.r = 0; color.b = 0; color.g = 0;

        const uint MIN = 255;
        while(color.r + color.b + color.g < MIN) {
            color.r = rand() % 256;
            color.g = rand() % 256;
            color.b = rand() % 256;
        }
        
        colors.push_back(color);
    } 

    jlib::x::Plot<T>::draw_point(point);

    i++;
}

void HyperPlot::draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2) {
    triple<int> color = colors[i-1];
    set_foreground(color.r, color.g, color.b);

    std::pair<uint,uint> mid; 
    mid.first = (p1.first + p2.first) / 2;
    mid.second = (p1.second + p2.second) / 2;

    jlib::x::Plot<T>::draw_line(p1, mid);
}

void key_pressed(std::string key,int x,int y, HyperPlot* plot) {
    if(key == "q") {
        exit(0);
    } else if(key == " ") {
        waiting = (!waiting);
    } else if(key == "w") {
        *plot * rotate;
        plot->draw();
    } else if(key == "s") {
        *plot * back_rotate;
        plot->draw();
    } else if(key == "e" || key == "d") {
        uint d = (key == "e" ? plot->D + 1 : plot->D - 1);
        cuboid<T> cube(d);

        for(uint i = 2; i < d; i++) {
            clip.push_back(std::make_pair(1.5, 4.5));
            trans.push_back(3); 
        }

        plot->setClip(clip);
        plot->change(d);
        initialize_rotation(plot);
        *plot * matrix<T>::translate(d, trans);
        plot->add(cube);
        plot->draw();
    }
}

void button_pressed(int button,int x,int y) {
}

void on_timeout(HyperPlot* plot) {
    if(!waiting) {
        *plot * rotate;
        plot->draw();
    }
}

void initialize_rotation(HyperPlot* plot) {
    rotate = matrix<T>::identity(plot->D + 1);
    back_rotate = matrix<T>::identity(plot->D + 1);

    std::vector<plane> planes;
    for(uint i = 0; i < plot->D; i++) {
        for(uint j = 0; j < plot->D; j++) {
            if(i < j && j < plot->D-1) {
                plane p; p.i = i; p.j = j;
                planes.push_back(p);
            }
        }
    }

    int d = 1;
    for(uint i = 0; i < planes.size(); i++) {
        double r = std::sin(d++) * PI / 180.0;
        rotate *= matrix<T>::rotate(plot->D, planes[i], r);
    }
    for(int i = planes.size()-1; i >=0; i--) {
        double r = -std::sin(--d) * PI / 180.0;
        back_rotate *= matrix<T>::rotate(plot->D,planes[i], r);
    }
}

int fac(int i) {
    int ret = 1;
    while(i > 0) {
        ret *= i--;
    }
    return ret;
}

int choose(int m, int k) {
    return fac(m) / (fac(k) * fac(m - k));
}
