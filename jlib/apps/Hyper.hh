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
 * TODO make it pretty.  turn on translucency alpha.  each vertex gets a color, and should be drawn as a glowing blob that is bigger than the edges.  the edges should blend the colors of the verticies in the line between them.  if am using HSV colors then i can just add the hues (letting them overflow) to get the color of the blended line.  make the lines thicker than they are currently, but the verticies should be larger.  change the brightness of the vertex/edge based on whats closer in 3d space.  this will require stopping at 3 dimensions to do this assay.
 * EVENTUALLY, MOVE THIS INTO VR
 */

#ifndef JLIB_APPS_HYPER_HH
#define JLIB_APPS_HYPER_HH

#include <jlib/apps/color.hh>
#include <jlib/math/math.hh>
#include <jlib/util/util.hh>

#include <cstdlib>

using namespace jlib;
using namespace jlib::math;

const long double PI = 3.14159265358979323846264338;

using apps::triple;
template<typename T, typename Plot>
class HyperPlot : public Plot {
public:
    enum Shape { CUBOID, PYRAMOID, STAROID, SPHEROID };

    HyperPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h);

    virtual void change(uint n);
    virtual void draw();
    virtual void draw_point(std::pair<uint,uint> point, uint index);
    virtual void draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2,
                           uint i1, uint i2);
    virtual void set_color(const triple<T>& color) = 0;

    void key_pressed(unsigned char key,int x,int y);
    void button_pressed(int button, int state, int x, int y);
    void on_timeout();

protected:
    void initialize(uint n);
    void initialize_rotation(uint n);
    void initialize_glazzies(uint n);

    /**
     * One hue per vertex, numbered across every object in the plot.
     *
     * See apps/color.hh: hues rather than triples so edges can blend without
     * washing out to grey, spaced by the golden ratio so neighbours stay
     * distinguishable and the figure comes up the same every run.
     */
    std::vector<T> hues;
    uint r;

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
      r(n),
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
    // Rotate in every plane, including those touching the highest axis.
    //
    // This defaulted to n-1, so a 4-cube's planes were only ever (0,1), (0,2)
    // and (1,2): nothing rotated through the fourth dimension, the two nested
    // cubes tumbled rigidly together, and the figure never everted -- which is
    // the entire thing a hypercube viewer exists to show.  The f key still
    // lowers it if you want the shell on its own.
    r = n;
    r2 = (n < 8 ? 3 : (1.1 * std::sqrt(static_cast<T>(n))));

    switch(shape) {
    case CUBOID: break;
    case PYRAMOID: break;
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
    case SPHEROID: break;
    }

    initialize_glazzies(n);
    this->setClip(clip);
    change(n);
    
    initialize_rotation(n);
    (*this) * matrix<T>::lookAt(n, eye, up, center);

    switch(shape) {
    case CUBOID: {
        cuboid<T> object(n);
        this->add(object);
        break;
    } 
    case PYRAMOID: {
        pyramoid<T> object(n);
        this->add(object);
        break;
    } 
    case STAROID: {
        staroid<T> object(n);
        this->add(object);
        break;
    } 
    case SPHEROID: {
        spheroid<T> object(n);
        this->add(object);
        break;
    } 
    }

}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::draw() {
    // Colours are per vertex and have to exist before anything is drawn.
    // draw_point used to generate them as it went, on the first frame only,
    // which needed a flag to say which frame that was and a counter to say
    // which vertex; an index on the primitives replaces both.
    uint n = 0;
    typename Plot::objref o = this->objects.begin();
    for(; o != this->objects.end(); o++)
        n += (*o)->size();

    while(hues.size() < n)
        hues.push_back(apps::golden_hue<T>(hues.size()));

    Plot::draw();
}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::change(uint n) {
    Plot::change(n);
    hues.clear();
}


template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::draw_point(std::pair<uint,uint> point, uint index) {
    this->set_color(apps::hsv(hues[index]));

    Plot::draw_point(point, index);
}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2,
                                  uint i1, uint i2) {
    // The whole edge, in the blend of its two endpoints.
    //
    // This used to draw half an edge, p1 to the midpoint, in p1's colour, and
    // rely on the edge being reached again from the other end to fill in the
    // rest.  The halves met at a hard seam in the middle, which is what made
    // the wireframe look assembled from pieces.
    //
    // Blending on the colour wheel rather than in RGB: averaging channels
    // drives every edge toward grey, which is what the old random triples did
    // whenever two colours were combined.
    std::vector<T> h;
    h.push_back(hues[i1]);
    h.push_back(hues[i2]);

    this->set_color(apps::hsv(apps::hue_mean(h)));

    Plot::draw_line(p1, p2, i1, i2);
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
    } else if(key == 'p') {
        // Cycle the projection: perspective -> orthographic -> mixed.
        //
        // Not a debug switch.  Perspective is what an N-dimensional camera
        // would see, but it destroys parallelism, and parallel edges are
        // exactly what makes an n-cube's structure readable.  Orthographic
        // keeps them.  Mixed divides on the outermost step only, so the
        // nesting from the highest dimension still shows while everything
        // below it stays parallel -- which is why it is the default.
        this->cycle_projection_mode();
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
