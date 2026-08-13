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
#include <jlib/math/dump.hh>

#include <cmath>
#include <iostream>

#include <vector>
#include <stack>


namespace jlib {
namespace math {

        
template<typename T>
class Plot {
public:
    enum STACK { MODELVIEW, PROJECTION };

    /**
     * How each step of the N->target reduction divides.
     *
     * project() builds an N-dimensional frustum whose last axis is depth, and
     * whose homogeneous coordinate is w = -x_{d-1}.  Whether that divide is
     * actually performed at a given step is a rendering choice, not a
     * correctness one:
     *
     *   perspective   divide at every step.  Physically what a camera in N
     *                 dimensions would see, and the most convincing.  It also
     *                 destroys parallelism, which is what makes the cell
     *                 structure hard to read.
     *
     *   orthographic  divide at none.  Parallel edges stay parallel, so the
     *                 combinatorial structure reads directly off the screen.
     *                 The standard choice for explaining an n-cube.
     *
     *   mixed         divide on the outermost step only.  The nesting from the
     *                 highest dimension shows as perspective, everything below
     *                 it stays orthographic -- so an inner and outer cube are
     *                 visibly the same cube offset in w, rather than merely
     *                 two cubes.  Legible and still shows the dimension you
     *                 care about.  This is jlib's long-standing behaviour and
     *                 the default.
     *
     * mixed was not chosen originally: it was the accidental result of
     * normalize() dividing by the wrong index after the first step.  It turned
     * out to be the most useful of the three for explaining these objects, so
     * it is now a named mode rather than a bug.
     */
    enum class projection_mode { perspective, orthographic, mixed };

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

    projection_mode get_projection_mode() const;
    void set_projection_mode(projection_mode m);

    /**
     * perspective -> orthographic -> mixed -> perspective.
     */
    projection_mode cycle_projection_mode();

    uint D;

    virtual void change(uint n);
    virtual void draw_point(std::pair<uint,uint> p) = 0;
    virtual void draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2)=0;

protected:
    bool visible(math::vertex<T> vertex) const;
    virtual math::vertex<T> transform(const math::vertex<T>& v) const;

    projection_mode m_projection;

    /**
     * Largest projected radius seen so far, used to scale map() to the window.
     *
     * The scale used to be fixed to the clip width, which only framed the
     * figure correctly for one projection mode and one D.  Every reduction
     * step divides by w, so perspective at D=5 lands around 0.03 where mixed
     * lands around 0.5 -- more than an order of magnitude, with nothing
     * compensating, so the figure shrank to a dot.
     *
     * Tracked as a running maximum, since the extent on any one frame is not
     * the largest the figure reaches as it turns.  Growing only means it
     * settles within a turn or two and cannot pump.  mutable because
     * transform() is const.
     */
    mutable T m_radius;

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
    : m_projection(projection_mode::mixed),
      m_radius(0),
      D(n),
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

    dump::frame_done();
}


template<typename T>
inline
std::pair<uint,uint> Plot<T>::map(const math::vertex<T>& v) {
    std::pair<uint,uint> ret;

    // Scale to whatever the projection actually produced rather than to the
    // clip width, so every mode and every D frame the same.  0.45 of the
    // smaller side leaves a margin for the corners, which swing wider than
    // the radius measured on any one frame.
    T mw, mh;
    if(m_radius > 0) {
        const T fit = 0.45 * ((width < height) ? width : height) / m_radius;
        mw = fit;
        mh = fit;
    }
    else {
        mw = width / (clip[0].second - clip[0].first);
        mh = height / (clip[1].second - clip[1].first);
    }

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

    // The outermost step: divided unless the mode is fully orthographic.
    if(m_projection != projection_mode::orthographic) {
        ret.normalize();
    }

    // Shrink to match the step just taken, so the next normalize() divides by
    // the new homogeneous coordinate rather than by the 1 this one left
    // behind.  Only in perspective: leaving it out is precisely what makes
    // every later step affine, which is what mixed is.
    if(m_projection == projection_mode::perspective) {
        ret.change(D - 1);
    }

    // Every step below it.  Note ret.change(d-1) after the divide: normalize()
    // divides by the vertex's own index D, and without shrinking the vertex to
    // match the step, that index still holds the 1 left by the previous step --
    // so the divide silently became a no-op.  That is what made every step
    // after the first orthographic by accident, which is now MIXED.
    for(int d = (D - 1); d > 2; d--) {
        math::matrix<T> p = math::matrix<T>::project(d, clip);
        math::vertex<T> v(d);
        v = ret;
        ret = p * v();

        if(m_projection == projection_mode::perspective) {
            ret.normalize();
            ret.change(d - 1);
        }
    }

    // Feeds map()'s framing; see m_radius.
    const T r = std::sqrt(ret[0] * ret[0] + ret[1] * ret[1]);
    if(r > m_radius)
        m_radius = r;

    dump::vertex("base", vertex, ret);

    return ret;
}

template<typename T>
inline
typename Plot<T>::projection_mode Plot<T>::get_projection_mode() const {
    return m_projection;
}

template<typename T>
inline
void Plot<T>::set_projection_mode(projection_mode m) {
    m_projection = m;
}

template<typename T>
inline
typename Plot<T>::projection_mode Plot<T>::cycle_projection_mode() {
    switch(m_projection) {
    case projection_mode::perspective:  m_projection = projection_mode::orthographic; break;
    case projection_mode::orthographic: m_projection = projection_mode::mixed;        break;
    case projection_mode::mixed:        m_projection = projection_mode::perspective;  break;
    }

    // The extent changes with the mode -- orthographic is more than an order
    // of magnitude larger than perspective -- so re-measure the framing.
    m_radius = 0;

    std::cerr << "projection: "
              << (m_projection == projection_mode::perspective  ? "perspective"  :
                  m_projection == projection_mode::orthographic ? "orthographic" : "mixed")
              << std::endl;

    return m_projection;
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
    m_radius = 0;
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
