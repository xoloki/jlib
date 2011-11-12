/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joe Yandle <jwy@divisionbyzero.com>
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

#ifndef JLIB_MATH_PLOT_HH
#define JLIB_MATH_PLOT_HH

#include <jlib/math/math.hh>

#include <vector>
#include <stack>


namespace jlib {
namespace math {

        
template<typename T>
class Plot {
public:
    enum STACK { MODELVIEW, PROJECTION };

    typedef typename std::list< object<T> >::iterator objref;

    Plot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h);
    virtual ~Plot() {}
    
    objref add(const math::object<T>& o);
    virtual void draw();

    std::pair<uint,uint> map(const math::vertex<T>& v);

    void set(STACK s);
    void push();
    void pop();

    void multiply(const math::matrix<T>& m);
    Plot& operator*(const math::matrix<T>& m);

    void setClip(std::vector< std::pair<T,T> > c);

    uint D;

    virtual void change(uint n);
    virtual void draw_point(std::pair<uint,uint> p) = 0;
    virtual void draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2)=0;

protected:
    bool visible(math::vertex<T> vertex) const;
    virtual math::vertex<T> transform(const math::vertex<T>& v) const;

    std::vector< std::pair<T,T> > clip;
    std::list< object<T> > objects;
    std::stack< matrix<T> > modelview;
    std::stack< matrix<T> > projection;
    STACK current;
    uint width;
    uint height;
};


template<typename T>
inline
Plot<T>::Plot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
    : D(n),
      clip(c),
      width(w),
      height(h)
{    
    if(c.size())
       change(n);
}


template<typename T>
inline
typename Plot<T>::objref Plot<T>::add(const object<T>& o) {
    return objects.insert(objects.end(), o);
}


template<typename T>
inline
void Plot<T>::draw() {
    typename std::list< object<T> >::iterator i = objects.begin();
    for(; i != objects.end(); i++) {
        std::map<vertex<T>, std::pair<uint,uint> > mapped;
        object<T>& object = *i;

        for(uint j = 0; j < object.size(); j++) {
            vertex<T>& v1 = object[j];
            vertex<T> tv1 = transform(v1);
            std::pair<uint,uint> p1 = map(tv1);

            mapped.insert(std::make_pair(v1, p1));
        }

        for(uint j = 0; j < object.size(); j++) {
            math::vertex<T>& v1 = object[j];
            //math::vertex<T>& tv1 = transformed.find(v1)->second;
            std::pair<uint,uint>& p1 = mapped.find(v1)->second;

            /* if(!visible(tv1)) continue; */

            draw_point(p1);
            
            std::list< math::vertex<T> > adjacent = object.adjacent(j);
            typename std::list< math::vertex<T> >::iterator k;
            for(k = adjacent.begin(); k != adjacent.end(); k++) {
                math::vertex<T>& v2 = *k;
                //math::vertex<T>& tv2 = transformed.find(v2)->second;
                std::pair<uint,uint>& p2 = mapped.find(v2)->second;

                /* if(!visible(tv2)) continue; */

                draw_line(p1, p2);
            }
        }
    }
}


template<typename T>
inline
std::pair<uint,uint> Plot<T>::map(const math::vertex<T>& v) {
    std::pair<uint,uint> ret;
    T cw = clip[0].second - clip[0].first;
    T ch = clip[1].second - clip[1].first;
    T mw = width / cw;
    T mh = height / ch;

    int cx = width / 2;
    int cy = height / 2;

    ret.first  = static_cast<uint>(cx + (mw * v[0]));
    ret.second = static_cast<uint>(cy + (mh * v[1]));

    return ret;
}


template<typename T>
inline
bool Plot<T>::visible(math::vertex<T> vertex) const {
    for(uint i = 0; i < D; i++) {
        T x = vertex[i];
        if(x < clip[i].first || x > clip[i].second) {
            return false;
        }
    }
    return true;
}


template<typename T>
inline
math::vertex<T> Plot<T>::transform(const math::vertex<T>& vertex) const {
    math::vertex<T> ret(D);

    ret = (projection.top() * modelview.top() * vertex());
    ret.normalize();

    // keep projecting until we get to two dimensions
    for(int d = (D - 1); d > 2; d--) {
        math::matrix<T> p = math::matrix<T>::project(d, clip);
        math::vertex<T> v(d);
        v = ret;
        ret = p * v();
        ret.normalize();
    }

    return ret;
}

template<typename T>
inline
void Plot<T>::set(STACK s) {
    current = s;
}

template<typename T>
inline
void Plot<T>::setClip(std::vector< std::pair<T,T> > c) {
    clip = c;
}

template<typename T>
inline
void Plot<T>::push() {
    switch(current) {
    case MODELVIEW:
        modelview.push(modelview.top());
        break;
    case PROJECTION:
        projection.push(projection.top());
        break;
    }
}


template<typename T>
inline
void Plot<T>::pop() {
    switch(current) {
    case MODELVIEW:
        modelview.pop();
        break;
    case PROJECTION:
        projection.pop();
        break;
    }
}


template<typename T>
inline
void Plot<T>::multiply(const math::matrix<T>& m) {
    switch(current) {
    case MODELVIEW:
        modelview.top() = modelview.top() * m;
        break;
    case PROJECTION:
        projection.top() = projection.top() * m;
        break;
    }
}


template<typename T>
inline
Plot<T>& Plot<T>::operator*(const math::matrix<T>& m) {
    multiply(m);
    return *this;
}


template<typename T>
inline
void Plot<T>::change(uint n) {
    D = n;
    while(modelview.size()) {
        modelview.pop();
    }
    while(projection.size()) {
        projection.pop();
    }
    modelview.push(math::matrix<T>::identity(D+1));
    projection.push(math::matrix<T>::identity(D+1));

    if(D > 2) {
        set(PROJECTION);
        multiply(math::matrix<T>::project(D, clip));
    }
    
    set(MODELVIEW);

    objects.clear();
}

}
}

#endif
