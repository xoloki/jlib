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

#ifndef JLIB_MATH_PLOT_HH
#define JLIB_MATH_PLOT_HH

#include <jlib/math/math.hh>
#include <jlib/math/dump.hh>

#include <cmath>
#include <iostream>

#include <memory>
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

    typedef typename std::list< std::shared_ptr< object<T> > >::iterator objref;

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

    void setClip(const std::vector< std::pair<T,T> >& c);

    projection_mode get_projection_mode() const;
    void set_projection_mode(projection_mode m);

    /**
     * perspective -> orthographic -> mixed -> perspective.
     */
    projection_mode cycle_projection_mode();

    uint D;

    virtual void change(uint n);
    /**
     * A vertex and an edge, each told which object vertices they came from.
     *
     * The index is what lets a subclass colour an endpoint.  Without it the
     * only thing an override could reach was a counter incremented by
     * draw_point, so an edge could only be drawn in the colour of whichever
     * end came through first -- which is why edges used to be drawn in halves.
     * Indices are numbered across every object in the plot.
     */
    virtual void draw_point(std::pair<uint,uint> p, uint index) = 0;
    virtual void draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2,
                           uint i1, uint i2) = 0;

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

    /**
     * One projection matrix per reduction step, built once and kept.
     *
     * project() depends only on the step's dimension and the clip volume,
     * neither of which changes within a frame, but it was being rebuilt for
     * every vertex: at D=10 that is seven matrices per vertex and 1024
     * vertices, so seven thousand matrices a frame where four are needed.
     *
     * Discarded whenever D or the clip changes, which is the only way either
     * can move.  mutable because transform() is const.
     */
    mutable std::vector< math::matrix<T> > m_project;

    /**
     * projection.top() * modelview.top(), built once and kept.
     *
     * transform() built this per vertex, and it is a (D+1)-square matrix
     * multiplied by another: O(D^3) for every corner.  At D=14 that is 3375
     * multiply-adds against 2475 for the whole reduction chain underneath it,
     * so the setup cost more than the work it was setting up.
     *
     * A vector for the same reason m_project is one -- matrix has no default
     * constructor -- and empty means "not built".  Discarded wherever the
     * stacks move: push(), pop(), multiply(), setClip() and change().  Nothing
     * outside this class touches either stack, which is what makes that list
     * complete rather than hopeful.
     */
    mutable std::vector< math::matrix<T> > m_mvp;

    /** Build m_project if it is empty. */
    void build_projections() const;

    /** Build m_mvp if it is empty. */
    void build_mvp() const;

    std::vector< std::pair<T,T> > clip;
    // Held by pointer so a shape keeps its type.  These used to be stored by
    // value, which sliced every subclass down to the base on the way in.
    std::list< std::shared_ptr< object<T> > > objects;
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
    return objects.insert(objects.end(), o.clone());
}


template<typename T>
inline
void Plot<T>::draw() {
    // Vertices are numbered across every object in the plot.
    uint base = 0;

    objref i = objects.begin();
    for(; i != objects.end(); i++) {
        object<T>& object = **i;

        // Indexed in parallel with the object's vertices.  This was a
        // std::map keyed by the vertex itself, which made coordinates the
        // identity of a vertex: two vertices at the same position collapsed
        // into one entry, and every lookup cost a tree search.  It also
        // dereferenced find() without checking it, so an adjacency naming a
        // vertex the object did not hold was undefined behaviour rather than
        // an error.
        std::vector< std::pair<uint,uint> > mapped(object.size());

        for(uint j = 0; j < object.size(); j++) {
            mapped[j] = map(transform(object[j]));
        }

        for(uint j = 0; j < object.size(); j++) {
            const std::pair<uint,uint>& p1 = mapped[j];

            /* if(!visible(tv1)) continue; */

            draw_point(p1, base + j);

            // Once per edge, not once per endpoint.  Adjacency is symmetric,
            // so walking it reaches every edge from both ends; that used to be
            // load-bearing, because half an edge was drawn from each end in
            // that end's colour and the two halves met in the middle.  An edge
            // now carries both its endpoints and is drawn whole, so the second
            // visit would be drawing it again.
            const std::vector<uint>& adjacent = object.adjacent(j);
            for(uint k = 0; k < adjacent.size(); k++) {
                if(adjacent[k] < j) continue;

                /* if(!visible(tv2)) continue; */

                draw_line(p1, mapped[adjacent[k]], base + j, base + adjacent[k]);
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

    build_mvp();

    ret = (m_mvp.front() * vertex());

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
    build_projections();

    for(int d = (D - 1); d > 2; d--) {
        // The temporary stays here, unlike in jhardhyper's copy of this loop.
        // change() below is conditional, so outside perspective mode ret keeps
        // its dimensionality while the steps shrink, and v is what truncates
        // ret to the step -- not merely a copy of it.
        math::vertex<T> v(d);
        v = ret;
        ret = m_project[D - d] * v();

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
void Plot<T>::setClip(const std::vector< std::pair<T,T> >& c) {
    clip = c;
    m_project.clear();
    m_mvp.clear();
}


template<typename T>
inline
void Plot<T>::build_projections() const {
    if(!m_project.empty())
        return;

    // Every step any reduction might take, indexed by D - d.  This one starts
    // at D-1, since its outermost step uses the projection stack rather than
    // project(); jhardhyper's starts at D.  Building the union is two extra
    // matrices a frame and keeps one index convention.
    for(int d = D; d > 2; d--)
        m_project.push_back(math::matrix<T>::project(d, clip));
}

template<typename T>
inline
void Plot<T>::build_mvp() const {
    if(!m_mvp.empty())
        return;

    m_mvp.push_back(projection.top() * modelview.top());
}

template<typename T>
inline
void Plot<T>::push() {
    m_mvp.clear();

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
    m_mvp.clear();

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
    m_mvp.clear();

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
    m_project.clear();
    while(modelview.size()) {
        modelview.pop();
    }
    while(projection.size()) {
        projection.pop();
    }
    modelview.push(math::matrix<T>::identity(D+1));
    projection.push(math::matrix<T>::identity(D+1));

    m_mvp.clear();

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
