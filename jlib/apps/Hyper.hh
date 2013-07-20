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

#ifndef JLIB_APPS_HYPER_HH
#define JLIB_APPS_HYPER_HH

#include <jlib/math/math.hh>

#include <cstdlib>

using namespace jlib;
using namespace jlib::math;

const long double PI = 3.14159265358979323846264338;

template<class O>
struct triple {
    O r;
    O g;
    O b;
};

template<typename T, typename Plot>
class HyperPlot : public Plot {
public:
    enum Shape { CUBOID, PYRAMOID, STAROID, SPHEROID };

    HyperPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h);

    virtual void change(uint n);
    virtual void draw();
    virtual void draw_point(std::pair<uint,uint> point);
    virtual void draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2);

    void key_pressed(unsigned char key,int x,int y);
    void button_pressed(int button, int state, int x, int y);
    void on_timeout();

protected:
    void initialize(uint n);
    void initialize_rotation(uint n);
    void initialize_glazzies(uint n);

    std::vector< triple<T> > colors;
    uint i;
    uint r;
    bool first;

    matrix<T> rotate;
    matrix<T> back_rotate;
    bool waiting;
    std::vector< std::pair<T,T> > clip;
    vertex<T> eye; 
    vertex<T> center; 
    vertex<T> up; 
    Shape shape;
    T r2;
};

template<typename T, typename Plot>
inline
HyperPlot<T,Plot>::HyperPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
    : Plot(n, c, w, h),
      r(n - 1),
      first(true),
      rotate(matrix<T>::identity(n+1)),
      back_rotate(matrix<T>::identity(n+1)),
      waiting(false),
      eye(n),
      center(n),
      up(n),
      shape(CUBOID)
{
    initialize(n);
}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::initialize(uint n) {
    r = n - 1;
    r2 = (n < 8 ? 3 : (1.1 * std::sqrt(static_cast<T>(n))));

    switch(shape) {
    case CUBOID: {
        break;
    } 
    case PYRAMOID: {
        break;
    } 
    case STAROID: {
        switch(n) {
        case 1:
        case 2:
            r2 = 6;
            break;
        case 3:
            r2 = 4.25;
            break;
        default:
            r2 = (2.1 * std::sqrt(static_cast<T>(n)));
            break;
        }
    }
    case SPHEROID: {
        break;
    } 
    }

    initialize_glazzies(n);
    setClip(clip);
    change(n);
    
    initialize_rotation(n);
    (*this) * matrix<T>::lookAt(n, eye, up, center);

    switch(shape) {
    case CUBOID: {
        cuboid<T> object(n);
        add(object);
        break;
    } 
    case PYRAMOID: {
        pyramoid<T> object(n);
        add(object);
        break;
    } 
    case STAROID: {
        staroid<T> object(n);
        add(object);
        break;
    } 
    case SPHEROID: {
        spheroid<T> object(n);
        add(object);
        break;
    } 
    }

}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::draw() {
    i = 0;

    Plot::draw();

    if(first) first = false;
}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::change(uint n) {
    Plot::change(n);
    first = true;
    colors.clear();
}


template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::draw_point(std::pair<uint,uint> point) {
    if(first) {
        triple<T> color; color.r = 0; color.b = 0; color.g = 0;

        const T MIN = 0.666;
        while(color.r + color.b + color.g < MIN) {
            color.r = static_cast<T>(std::rand() % 256) / 255.0;
            color.g = static_cast<T>(std::rand() % 256) / 255.0;
            color.b = static_cast<T>(std::rand() % 256) / 255.0;
        }
        
        colors.push_back(color);
    } 

    Plot::draw_point(point);

    i++;
}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2) {
    triple<T> color = colors[i-1];
    //set_foreground(color.r, color.g, color.b);
    GLfloat fcolors[4];
    fcolors[0] = color.r; fcolors[1] = color.g; fcolors[2] = color.b; fcolors[3] = 1.0;
    glColor3fv(fcolors);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, fcolors);

    std::pair<uint,uint> mid; 
    mid.first = (p1.first + p2.first) / 2;
    mid.second = (p1.second + p2.second) / 2;

    Plot::draw_line(p1, mid);
}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::key_pressed(unsigned char key, int x, int y) {
    if(key == 'q') {
        exit(0);
    } else if(key == ' ') {
        waiting = (!waiting);
    } else if(key == 'w') {
        *this * rotate;
        //plot->draw();
    } else if(key == 's') {
        *this * back_rotate;
        //plot->draw();
    } else if(key == 'e' || key == 'd') {
        uint d = (key == 'e' ? this->D + 1 : this->D - 1);

        if(d < 1)
            return;

        initialize(d);
    } else if(key == 'r' || key == 'f') {
        uint nr = (key == 'r' ? r + 1 : r - 1);

        if(nr < 0 || nr > this->D)
            return;

        r = nr;
        initialize_rotation(this->D);
    } else if(key == 't' || key == 'g') {
        int x = static_cast<int>(shape);
        
        x += (key == 't' ? 1 : -1);
        
        if(x > static_cast<int>(SPHEROID))
            x = static_cast<int>(CUBOID);
        else if(x < CUBOID) {
            x = static_cast<int>(SPHEROID);
        }
        
        shape = static_cast<Shape>(x);
    
        initialize(this->D);
/*
    } else if(key == 'y' || key == 'h') {
        T x = (key == 't' ? 0.1 : -0.1);
        r2 += x;

        initialize_glazzies(this->D);
        initialize_rotation(this->D);
        (*this) * matrix<T>::lookAt(this->D, eye, up, center);
*/
    }
}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::button_pressed(int button, int state, int x, int y) {
}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::on_timeout() {
    if(!waiting) {
        *this * rotate;
    }
}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::initialize_rotation(uint n) {
    rotate = matrix<T>::identity(n + 1);
    back_rotate = matrix<T>::identity(n + 1);

    std::vector<plane> planes;
    for(uint i = 0; i < n; i++) {
        for(uint j = 0; j < n; j++) {
            if(i < j && j < r) { // change this to n to rotate in all dimensions
                plane p; p.i = i; p.j = j;
                planes.push_back(p);
            }
        }
    }

    int d = 1;
    for(uint i = 0; i < planes.size(); i++) {
        double r = std::sin(d++) * PI / 180.0;
        rotate *= matrix<T>::rotate(n, planes[i], r);
    }
    for(int i = planes.size()-1; i >=0; i--) {
        double r = -std::sin(--d) * PI / 180.0;
        back_rotate *= matrix<T>::rotate(n,planes[i], r);
    }
}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::initialize_glazzies(uint n) {
    T r22 = r2 / 2;

    clip.clear();
    clip.push_back(std::make_pair(-r22, r22));
    clip.push_back(std::make_pair(-r22, r22));
    
    eye.change(n);
    eye[0] = 0;
    eye[1] = 0;

    for(uint i = 2; i < n; i++) {
        clip.push_back(std::make_pair(r22, 3*r22));
        eye[i] = r2;
    }
}

#endif
