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
 * XXX TODO fix this by printing the matrix contents out in a machine readable format.  correct for the different column layout, then get the same results from jhyper.  be rotating in all planes from the beginning. at some point the two modelview matricies will diverge, use this as a debugger to find out when/where it broke
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

#include <cstdlib>

#include <unistd.h>


#include <jlib/math/math.hh>
#include <jlib/math/Plot.hh>
#include <jlib/math/dump.hh>
#include <jlib/glfw/Plot.hh>

#include <vector>
#include <stack>


// The file-local copy of jlib::glut::Plot<T> that used to sit here -- ~100
// lines duplicated from jlib/glut/Plot.hh -- is replaced by glfw::Plot<T>.
// What remains forked is HyperPlot below, which exists only because the
// shared Hyper.hh reduces all the way to 2D; see issue #20.


using namespace jlib;
using namespace jlib::math;

typedef GLdouble T;

const long double PI = 3.14159265358979323846264338;




template<typename T>
class HPlot : public glfw::Plot<T> {
public:
    HPlot(uint n, std::vector< std::pair<T,T> > c, uint w=400, uint h=400);

    virtual void draw();

    /**
     * A vertex and an edge, each told which object vertices they came from.
     *
     * The index is what lets a subclass colour an endpoint.  Without it the
     * only thing an override could reach was a counter incremented by
     * draw_point, so an edge could only be drawn in the colour of whichever
     * end came first -- which is why edges used to be drawn in halves.
     */
    virtual void draw_point(const math::vertex<T>& p, uint index);
    virtual void draw_line(const math::vertex<T>& p1, const math::vertex<T>& p2,
                           uint i1, uint i2);

    /**
     * One face, as a polygon with a normal.
     *
     * index[k] is the object vertex that corner[k] came from, numbered across
     * every object in the plot, so a subclass can colour a face from whatever
     * it keeps per vertex.  The geometry here ignores it.
     */
    virtual void draw_face(const std::vector< math::vertex<T> >& corner,
                           const math::vertex<T>& normal,
                           const std::vector<uint>& index);

    /** Solid faces as well as the wireframe.  The o key toggles it. */
    bool m_solid = true;

protected:
    virtual math::vertex<T> transform(const math::vertex<T>& v) const;

    // This one reduces N->3 and hands GL real 3-D coordinates, so it needs a
    // perspective projection rather than the base's pixel-space ortho2d.
    // glut::Main::default_reshape used gluPerspective(80, aspect, 0.1, 50),
    // which is what glu::projection::perspective sets up.
    virtual void on_configure(int width, int height);

    /**
     * Distance to put the GL camera at, worked out from how big the object
     * actually came out.
     *
     * It cannot be a constant.  Every reduction step divides by w, so the
     * projected object shrinks by roughly half per step: a 4-cube lands at
     * half-extent 0.5 and a 5-cube at about 0.25, while a fixed camera stays
     * put -- so the figure receded as D went up.
     *
     * Tracked as a running maximum rather than measured once.  The extent at
     * frame zero is not the largest the figure reaches as it turns, so a
     * one-shot measurement frames it too tightly and the corners clip.  Only
     * ever moving back means it settles within a turn or two and never
     * breathes.  change() resets it, so the e and d keys re-frame.
     */
    T m_camera = 0;
    T m_radius = 0;

    /**
     * Zoom, as a multiplier on how much of the window the figure fills.
     *
     * Survives change(), unlike the measured framing: how close you want to
     * sit is a preference, not a property of the figure.
     */
    T m_zoom = 1.0;

    /**
     * Discard the measured framing so the next frame re-measures.  Needed
     * whenever the projection changes shape -- a different mode or a
     * different D both change how big the result comes out.
     */
    void reframe() { m_camera = 0; m_radius = 0; }

    virtual void change(uint n);
};

template<typename T>
inline
void HPlot<T>::change(uint n) {
    // D is about to change, so the projected extent will too -- and it shrinks
    // by roughly half for every dimension added.  Re-measure from scratch.
    reframe();

    glfw::Plot<T>::change(n);
}

template<typename T>
inline
HPlot<T>::HPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
    : glfw::Plot<T>(n, c, w, h)
{
    // Always perspective, and there is no key to change it.
    //
    // This app exists to split the work: reduce N->3 in software and let the
    // hardware do 3->2.  That makes the other two modes either meaningless or
    // dishonest here.  Mixed is defined by leaving the inner reduction steps
    // affine, and the inner step is GL's, not ours.  Orthographic looks right
    // at D=4 -- the single software step skips its divide -- but GL still
    // applies perspective underneath, so it is not really orthographic at all,
    // which becomes obvious above D=4.
    //
    // jglfwhyper is where the projection modes are demonstrated: it reduces
    // all the way to 2-D in software, so a mode there means what it says.
    // This one is where the solid rendering is.
    this->set_projection_mode(math::Plot<T>::projection_mode::perspective);

    // glfw::Plot leaves lighting alone, because a wireframe has no normals and
    // enabling it there only makes glColor inert.  Faces do have normals, so
    // this one needs it -- in the two-sided form, since a projected hypercube
    // shows both sides of every face.  It stays disabled except during the
    // face pass, so the edges and points keep their flat colours.
    gl::lights::init(true);
    glDisable(GL_LIGHTING);
}

template<typename T>
inline
void HPlot<T>::on_configure(int width, int height) {
    this->width = width;
    this->height = height;

    glu::projection::perspective(width, height);

    this->draw();
}

template<typename T>
inline
void HPlot<T>::draw_point(const math::vertex<T>& p, uint) {
    glBegin(GL_POINTS);
    //glVertex3d(p[0], p[1], p[2]);
    glVertex4dv(p.data());
    glEnd();
}

template<typename T>
inline
void HPlot<T>::draw_line(const math::vertex<T>& p1, const math::vertex<T>& p2,
                         uint, uint) {
    glBegin(GL_LINES);
    //std::cout << "HPlot<T>::draw_line: p1.D: " << p1.D << "\n" << static_cast< math::matrix<T> >(p1) << std::endl
    //          << "p2.D: " << p2.D << "\n" << static_cast< math::matrix<T> >(p2) << std::endl;
    glVertex4dv(p1.data());
    glVertex4dv(p2.data());
    //glVertex4d(p1[0], p1[1], p1[2], p1[3]);
    //glVertex4d(p2[0], p2[1], p2[2], p2[3]);
    //glVertex3d(p1[0], p1[1], p1[2]);
    //glVertex3d(p2[0], p2[1], p2[2]);
    glEnd();
}

template<typename T>
inline
void HPlot<T>::draw_face(const std::vector< math::vertex<T> >& corner,
                         const math::vertex<T>& normal,
                         const std::vector<uint>&) {
    glNormal3d(normal[0], normal[1], normal[2]);

    glBegin(GL_POLYGON);
    for(uint k = 0; k < corner.size(); k++)
        glVertex4dv(corner[k].data());
    glEnd();
}

/**
 * The normal of a projected face, or false if it has none.
 *
 * A function of where the face landed, not of the source geometry: the object
 * turns in N dimensions and re-projects every frame, so the 3-D orientation of
 * a face is only known after the reduction.
 *
 * Faces going edge-on is routine rather than exceptional here -- a hypercube
 * shows several every turn, and a whole cell flattens whenever the rotation
 * takes it perpendicular to the projection.  The cross product vanishes there
 * and the direction is genuinely undefined, so those are skipped.
 */
template<typename T>
inline
bool face_normal(const std::vector< math::vertex<T> >& corner,
                 math::vertex<T>& normal) {
    if(corner.size() < 3)
        return false;

    T a[3], b[3];
    for(uint k = 0; k < 3; k++) {
        a[k] = corner[1][k] - corner[0][k];
        b[k] = corner[corner.size() - 1][k] - corner[0][k];
    }

    T n[3];
    n[0] = a[1] * b[2] - a[2] * b[1];
    n[1] = a[2] * b[0] - a[0] * b[2];
    n[2] = a[0] * b[1] - a[1] * b[0];

    const T len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

    // Purely relative, with no absolute floor.
    //
    // len is an area, and the figure shrinks by roughly a third per dimension
    // as each reduction step divides by w -- so the area of a face falls by
    // about a factor of ten per dimension.  An absolute cutoff of 1e-6 is
    // nowhere near the geometry at D=4, where a face measures 2e-1, but it
    // passes straight through it around D=8 and swallows the figure whole:
    // measured, it discarded 60% of the faces at D=9, 99.7% at D=10 and every
    // last one from D=11 up.  That is what made the solid disappear at high D
    // while the wireframe carried on.
    //
    // Dividing by the edge lengths gives the sine of the angle between them,
    // which says whether the face has a direction without saying anything
    // about its size.  That is the actual question, and it rejected nothing
    // at any D from 4 to 12 except genuinely edge-on faces.
    const T scale = std::sqrt((a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) *
                              (b[0] * b[0] + b[1] * b[1] + b[2] * b[2]));

    if(scale <= 0 || len / scale < 1e-6)
        return false;

    for(uint k = 0; k < 3; k++)
        normal[k] = n[k] / len;

    return true;
}

template<typename T>
inline
void HPlot<T>::draw() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Vertices are numbered across every object, matching the counter
    // draw_point keeps, so a subclass can index one colour table with either.
    uint base = 0;

    typename math::Plot<T>::objref i = math::Plot<T>::objects.begin();
    for(; i != math::Plot<T>::objects.end(); i++) {
        math::object<T>& object = **i;

        // Parallel to the object's vertices; see the note in math::Plot::draw.
        // Built by push_back rather than sized up front: vertex<T> has no
        // default constructor, since a vertex has no meaning without a
        // dimensionality.
        std::vector< math::vertex<T> > transformed;
        transformed.reserve(object.size());

        for(uint j = 0; j < object.size(); j++) {
            transformed.push_back(transform(object[j]));
        }

        // Frame the object: far enough back that the outermost vertex sits
        // inside the field, growing the distance if a later rotation reaches
        // further than anything seen so far.
        T radius = 0;
        for(uint j = 0; j < transformed.size(); j++) {
            const math::vertex<T>& v = transformed[j];
            T r2 = 0;
            for(uint k = 0; k < 3 && k < v.D; k++)
                r2 += v[k] * v[k];
            if(r2 > radius) radius = r2;
        }
        radius = std::sqrt(radius);

        if(radius > m_radius)
            m_radius = radius;

        // Scale the figure to a fixed size rather than moving the camera to
        // meet it.
        //
        // Chasing it with the camera works until about D=7: each reduction
        // step divides by w, so by then the object is down to a radius of
        // ~0.06, the camera closes to ~0.12, and the near half of the figure
        // crosses gluPerspective's near plane at 0.1 and clips.  Normalizing
        // instead keeps the camera at a comfortable distance for every D, and
        // has the object appear the same size throughout.
        const T scale = (m_radius > 0) ? (1.0 / m_radius) : 1.0;

        // Put the camera where the figure fills the fraction of the window we
        // asked for.
        //
        // For a unit-radius figure at the origin and a camera at distance d on
        // +z with vertical half-angle t, the widest any vertex can project is
        // 1/(sqrt(d^2-1)*tan(t)) of the half-field.  That maximum is not at
        // the silhouette: it falls at cos = 1/d, because a vertex nearer the
        // camera projects wider than its radius alone would suggest.
        // Inverting for a desired fill f gives the line below, where f = 1 has
        // the figure's diameter exactly span the window height.
        //
        // This used to read d = 1/(0.6*tan(t)), which looks like 60% fill and
        // is not: it works out to 69% at the figure's widest moment, and since
        // m_radius is a running maximum most frames sit well under their own
        // maximum -- landing around half the window, which is how it looked.
        //
        // Clamped below 1.8 because the figure has unit radius and
        // gluPerspective's near plane is at 0.1: past there the camera closes
        // to within 1.1 of the centre and the near face crosses the plane.
        const T half_field = std::tan(40.0 * PI / 180.0);
        const T fill = std::min(std::max(0.9 * m_zoom, 0.1), 1.8);
        const T reach = fill * half_field;

        m_camera = std::sqrt(1.0 + 1.0 / (reach * reach));

        // The 3-D camera, set here rather than through the N-D lookAt so it
        // is not scaled by the perspective divide.  See initialize_glazzies.
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        gluLookAt(0, 0, m_camera,
                  0, 0, 0,
                  0, 1, 0);
        glScaled(scale, scale, scale);

        // Depth cue: fade toward the background with distance.
        //
        // Everything is translucent and drawn from every angle, so without
        // this the far side of the figure is exactly as bright as the near
        // side and the eye has nothing to order them by -- which is the one
        // thing stopping the reduction at three dimensions was supposed to
        // buy.  The figure is normalized to unit radius, so it spans one
        // either side of the camera's focus; the far bound goes a little past
        // that so the back does not go fully black.
        const GLfloat fog[4] = { 0, 0, 0, 1 };
        glFogfv(GL_FOG_COLOR, fog);
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogf(GL_FOG_START, static_cast<GLfloat>(m_camera - 1.0));
        glFogf(GL_FOG_END, static_cast<GLfloat>(m_camera + 1.8));
        glEnable(GL_FOG);

        // Solid faces first, then the wireframe over them.
        //
        // Everything here is translucent, so there is no opaque pass to
        // establish depth against.  The faces sort back to front and do not
        // write depth, which is what makes overlapping faces blend in the
        // right order; the edges and points then draw against an untouched
        // depth buffer and none of them are hidden.  Losing the wireframe
        // inside the solid would defeat the point -- the edges are how you
        // follow a cell through an eversion.
        if(m_solid && math::Plot<T>::D >= 3 && !object.get_faces().empty()) {
            const std::vector<typename math::object<T>::face_type>& faces =
                object.get_faces();

            // Depth of a face is its centroid's z.  The modelview applies a
            // uniform positive scale and a translation along z, so ordering
            // by z here is the same as ordering by eye-space depth.
            std::vector< std::pair<T,uint> > order;
            order.reserve(faces.size());

            for(uint f = 0; f < faces.size(); f++) {
                T z = 0;
                for(uint k = 0; k < faces[f].size(); k++)
                    z += transformed[faces[f][k]][2];

                order.push_back(std::make_pair(z / faces[f].size(), f));
            }

            // ascending z: the camera looks down -z, so smallest is farthest
            std::sort(order.begin(), order.end());

            glEnable(GL_LIGHTING);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_CULL_FACE);
            glDepthMask(GL_FALSE);

            std::vector< math::vertex<T> > corner;
            std::vector<uint> index;
            math::vertex<T> normal(3);

            for(uint o = 0; o < order.size(); o++) {
                const typename math::object<T>::face_type& face =
                    faces[order[o].second];

                corner.clear();
                index.clear();
                for(uint k = 0; k < face.size(); k++) {
                    corner.push_back(transformed[face[k]]);
                    index.push_back(base + face[k]);
                }

                if(face_normal(corner, normal))
                    draw_face(corner, normal, index);
            }

            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glDisable(GL_LIGHTING);
        }

        // Smooth shading so an edge blends between its endpoints' colours
        // along its length, and antialiasing so it does not come out as a
        // jagged hairline over the faces.
        glShadeModel(GL_SMOOTH);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LINE_SMOOTH);
        glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
        glEnable(GL_POINT_SMOOTH);
        glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
        glLineWidth(2.0);
        glPointSize(5.0);

        for(uint j = 0; j < object.size(); j++) {
            draw_point(transformed[j], base + j);

            // Once per edge, not once per endpoint.  Adjacency is symmetric,
            // so walking it visits every edge from both ends; that used to be
            // load-bearing, because half an edge was drawn from each end in
            // that end's colour.  An edge now carries both endpoints and
            // blends across, so the second visit is pure overdraw.
            const std::vector<uint>& adjacent = object.adjacent(j);
            for(uint k = 0; k < adjacent.size(); k++) {
                if(adjacent[k] < j) continue;

                draw_line(transformed[j], transformed[adjacent[k]],
                          base + j, base + adjacent[k]);
            }
        }

        glDisable(GL_POINT_SMOOTH);
        glDisable(GL_LINE_SMOOTH);
        glDisable(GL_BLEND);

        base += object.size();
    }

    glDisable(GL_FOG);

	this->flush();

    math::dump::frame_done();
}

template<typename T>
inline
math::vertex<T> HPlot<T>::transform(const math::vertex<T>& vertex) const {
    math::vertex<T> ret(math::Plot<T>::D);

    ret = math::Plot<T>::modelview.top() * vertex();
    //ret = vertex;
    //ret.normalize();
    //ret.change(ret.D - 1);

    // Reduce to three dimensions and hand those to GL.
    //
    // Which steps divide is the projection mode: every one for perspective,
    // none for orthographic, and only the outermost for mixed.
    typedef typename math::Plot<T>::projection_mode mode;
    const mode m = this->get_projection_mode();

    for(int d = math::Plot<T>::D; d > 3; d--) {
        math::matrix<T> p = math::matrix<T>::project(d, math::Plot<T>::clip);
        math::vertex<T> v(d);
        v = ret;
        ret = p * v();

        const bool outermost = (d == static_cast<int>(math::Plot<T>::D));
        if(m == mode::perspective || (m == mode::mixed && outermost)) {
            ret.normalize();
        }

        ret.change(d-1);
    }

    math::dump::vertex("hard", vertex, ret);

    return ret;
}

typedef HPlot<T> PlotType;

template<class O>
struct triple {
    O r;
    O g;
    O b;
};

/**
 * A hue, at full saturation and value.
 *
 * Colours are kept as hues rather than triples so they can be combined
 * without washing out.  Averaging in RGB drives every channel toward its
 * mean: four random corners average to a standard deviation of about 0.14
 * around 0.5, so every face came out the same olive grey no matter which
 * vertices it joined.  A hue has nowhere to collapse to -- the result of
 * combining hues is always another fully saturated colour.
 */
template<typename O>
inline
triple<O> hsv(O h) {
    h -= std::floor(h);

    const O sector = h * 6;
    const int i = static_cast<int>(std::floor(sector));
    const O f = sector - i;

    const O q = 1 - f;
    const O t = f;

    triple<O> c;
    switch(i % 6) {
    case 0: c.r = 1; c.g = t; c.b = 0; break;
    case 1: c.r = q; c.g = 1; c.b = 0; break;
    case 2: c.r = 0; c.g = 1; c.b = t; break;
    case 3: c.r = 0; c.g = q; c.b = 1; break;
    case 4: c.r = t; c.g = 0; c.b = 1; break;
    default: c.r = 1; c.g = 0; c.b = q; break;
    }

    return c;
}

/**
 * Where a set of hues sits on the colour wheel.
 *
 * The mean direction, not the arithmetic mean: hues wrap, so averaging 0.9
 * and 0.1 numerically gives 0.5 -- cyan from two reds.  Summing unit vectors
 * and taking the angle gets the answer that agrees with the wheel.
 *
 * Corners spread evenly around the wheel sum to nothing and have no mean
 * direction at all.  That is a real ambiguity rather than a numerical one, so
 * it falls back to the first corner instead of pretending otherwise.
 */
template<typename O>
inline
O hue_mean(const std::vector<O>& h) {
    if(h.empty()) return 0;

    O x = 0, y = 0;
    for(uint k = 0; k < h.size(); k++) {
        x += std::cos(2 * PI * h[k]);
        y += std::sin(2 * PI * h[k]);
    }

    if(std::sqrt(x * x + y * y) < 1e-9)
        return h[0];

    const O a = std::atan2(y, x) / (2 * PI);

    return a - std::floor(a);
}

template<typename T, typename Plot>
class HyperPlot : public Plot {
public:
    HyperPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h);

    virtual void change(uint n);
    virtual void draw();
    virtual void draw_point(const math::vertex<T>& point, uint index);
    virtual void draw_line(const math::vertex<T>& p1, const math::vertex<T>& p2,
                           uint i1, uint i2);
    virtual void draw_face(const std::vector< math::vertex<T> >& corner,
                           const math::vertex<T>& normal,
                           const std::vector<uint>& index);

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
     * Spaced by the golden ratio rather than chosen at random.  Successive
     * hues then land as far apart on the wheel as they can, so neighbouring
     * vertices stay distinguishable at any D, and the figure comes up the
     * same colours every run -- which matters when comparing two builds.
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
      up(n)
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

    initialize_glazzies(n);
    this->setClip(clip);
    change(n);
    
    initialize_rotation(n);
    (*this) * matrix<T>::lookAt(n, eye, up, center);

    cuboid<T> object(n);
    this->add(object);
}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::draw() {
    // Colours are per vertex and have to exist before anything is drawn.
    // draw_point used to generate them as it went, on the first frame only,
    // which worked while points were the first thing drawn -- the face pass
    // now runs ahead of them and would have read an empty table.
    uint n = 0;
    typename Plot::objref o = this->objects.begin();
    for(; o != this->objects.end(); o++)
        n += (*o)->size();

    while(hues.size() < n) {
        const T golden = 0.6180339887498948;

        hues.push_back(std::fmod(hues.size() * golden, 1.0));
    }

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
void HyperPlot<T,Plot>::draw_point(const math::vertex<T>& point, uint index) {
    const triple<T> c = hsv(hues[index]);

    glColor4f(c.r, c.g, c.b, 1.0);

    Plot::draw_point(point, index);
}


/**
 * A face in the blend of its corners' colours, translucent.
 *
 * No attempt to wind the faces outward: a hypercube reduced to three
 * dimensions is a shadow rather than a solid, so faces turn inside out as it
 * everts and both sides are seen.  Two-sided lighting and a material set on
 * GL_FRONT_AND_BACK are what make that come out right regardless of which way
 * a given face happens to be facing this frame.
 */
template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::draw_face(const std::vector< math::vertex<T> >& corner,
                                  const math::vertex<T>& normal,
                                  const std::vector<uint>& index) {
    std::vector<T> h;
    for(uint k = 0; k < index.size(); k++)
        if(index[k] < hues.size())
            h.push_back(hues[index[k]]);

    const triple<T> color = hsv(hue_mean(h));

    // Low enough that a 4-cube is four or five faces deep in places and still
    // reads as separate surfaces rather than a solid block.
    GLfloat fcolors[4];
    fcolors[0] = color.r; fcolors[1] = color.g; fcolors[2] = color.b;
    fcolors[3] = 0.18;

    glColor4fv(fcolors);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, fcolors);

    Plot::draw_face(corner, normal, index);
}

template<typename T, typename Plot>
inline
void HyperPlot<T,Plot>::draw_line(const math::vertex<T>& p1, const math::vertex<T>& p2,
                                  uint i1, uint i2) {
    // A colour at each end and let the rasterizer interpolate.
    //
    // This used to draw half an edge, from p1 to the midpoint, in p1's
    // colour, and rely on the edge being visited again from the other end to
    // fill in the rest.  The two halves met at the midpoint with a hard seam,
    // which is what made the wireframe look like it was assembled from
    // separate pieces.  Interpolating across the whole edge blends the two
    // hues the way the 2021 design asked for, and needs one pass.
    const triple<T> c1 = hsv(hues[i1]);
    const triple<T> c2 = hsv(hues[i2]);

    glBegin(GL_LINES);
    glColor4f(c1.r, c1.g, c1.b, 1.0);
    glVertex4dv(p1.data());
    glColor4f(c2.r, c2.g, c2.b, 1.0);
    glVertex4dv(p2.data());
    glEnd();
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
        this->draw();
    } else if(key == 's') {
        *this * back_rotate;
        this->draw();
    } else if(key == 'e' || key == 'd') {
        uint d = (key == 'e' ? this->D + 1 : this->D - 1);

        if(d < 1)
            return;

        initialize(d);
    } else if(key == 'z' || key == 'x') {
        this->m_zoom *= (key == 'z' ? 1.15 : 1.0 / 1.15);
        this->draw();
    } else if(key == 'o') {
        this->m_solid = !this->m_solid;
        this->draw();
    } else if(key == 'r' || key == 'f') {
        uint nr = (key == 'r' ? r + 1 : r - 1);

        if(nr < 0 || nr > this->D)
            return;

        r = nr;
        initialize_rotation(this->D);
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
    const T r2 = 3;
    const T r22 = r2 / 2;

    clip.clear();
    clip.push_back(std::make_pair(-r22, r22));
    clip.push_back(std::make_pair(-r22, r22));

    eye.change(n);
    eye[0] = 0;
    eye[1] = 0;

    up.change(n);
    up[0] = 0;
    up[1] = 1;

    center.change(n);
    center[0] = 0;
    center[1] = 0;

    // Axis 2 is not projected away here: this reduction stops at three
    // dimensions and hands x, y, z to OpenGL.  So its clip entry is only ever
    // a frustum extent, and its eye offset exists to put the object in front
    // of the GL camera, which sits at the origin looking down -z.
    // Axis 2 gets NO eye offset, unlike the axes above it.
    //
    // This reduction stops at three dimensions and hands x, y, z to OpenGL,
    // which applies its own perspective.  Putting the 3-D camera distance
    // into the N-D lookAt means it is applied *before* the 4-D divide and
    // scaled by it -- and then GL divides x,y by that already-divided z, so
    // the w cancels out exactly and the 4-D perspective has no effect on
    // apparent size.  The inner and outer cubes come out identical.
    //
    // The 3-D camera belongs in GL, after the reduction; see on_configure.
    if(n > 2) {
        clip.push_back(std::make_pair(-r22, r22));
        eye[2] = 0;
        up[2] = 0;
        center[2] = 0;
    }

    // Every axis above the third *is* projected away, and each needs a
    // frustum wholly in front of the eye.
    //
    // This used to push (-3, 3) -- a negative near plane -- and leave eye[i]
    // at 0 for every axis but the second.  project() yields w = -x_{d-1}, so
    // a hypercube spanning +-1 about an eye of 0 gives w = -+1: the
    // perspective divide then flips sign for half the vertices and mirrors
    // them through the origin, with the two halves straddling the camera.
    for(uint i = 3; i < n; i++) {
        clip.push_back(std::make_pair(r22, 3*r22));
        eye[i] = r2;
        up[i] = 0;
        center[i] = 0;
    }
}


class HardPlot : public HyperPlot<T, PlotType> {
public:
    HardPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
        : HyperPlot<T, PlotType>(n, c, w, h)
    {
        // Per-object signals, the same surface jhyper and jglfwhyper bind.
        key_press.connect([this](auto&&... a) { return this->key_pressed(a...); });
        button_press.connect([this](auto&&... a) { return this->button_pressed(a...); });
        timeout.connect([this](auto&&... a) { return this->on_timeout(a...); });
    }

    void on_timeout() {
        HyperPlot<T, PlotType>::on_timeout();

        if(waiting)
            std::this_thread::sleep_for(std::chrono::microseconds(100000));
        else
            this->draw();
    }

    // GLFW reports typed text, as X does; HyperPlot wants a single char.
    void key_pressed(std::string key, int x, int y) {
        if(key.empty())
            return;

        HyperPlot<T, PlotType>::key_pressed(key[0], x, y);
    }

    void button_pressed(int button, int x, int y) {
        HyperPlot<T, PlotType>::button_pressed(button, 0, x, y);
    }
};

int main(int argc, char** argv) {
    uint D = 5;
    if(argc > 1) {
        D = atoi(argv[1]);
    }

    try {
        HardPlot plot(D, std::vector< std::pair<T,T> >(), 700, 700);

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

