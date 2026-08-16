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


#include <jlib/apps/color.hh>
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

using apps::PI;




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
     * One face, as a polygon with a normal per corner.
     *
     * normal is 3 values per corner, parallel to corner: flat shading repeats
     * the face's own normal, smooth shading gives each corner the mean of the
     * faces meeting there.
     *
     * index[k] is the object vertex that corner[k] came from, numbered across
     * every object in the plot, so a subclass can colour a face from whatever
     * it keeps per vertex.  The geometry here ignores it.
     */
    virtual void draw_face(const std::vector< math::vertex<T> >& corner,
                           const std::vector<T>& normal,
                           const std::vector<uint>& index);

    /** Solid faces as well as the wireframe.  The o key toggles it. */
    bool m_solid = true;

    /**
     * Draw the edges and vertices over the faces.
     *
     * Right when the edges are the structure, as on a hypercube, where they
     * are 2% of the projected surface.  A tessellated surface's edges are
     * just where the mesh was cut: a 32x32 torus puts 2048 of them over the
     * same area, covering 21% of it, and what you see is the mesh rather than
     * the shape.  Set from the shape, and the l key overrides it.
     */
    bool m_wire = true;

    /**
     * Per-face alpha.
     *
     * A hypercube is a shadow to see into; a surface is a surface, and wants
     * to read as one while still showing where it passes through itself.
     */
    T m_alpha = 0.18;

    /**
     * Hold the total accumulated opacity constant instead of the per-face
     * value, using how deeply the faces actually stack this frame.
     *
     * A 2-torus is a surface: 1.8 faces along a line of sight, so what you
     * set is roughly what you get.  A 3-torus is not a surface -- its faces
     * are interior scaffold, 18 deep at the coarsest useful mesh -- and the
     * same alpha there accumulates to 0.97 and hides everything it is meant
     * to show.  Solving 1-(1-a)^depth for a fixed total gives
     * a = 1-(1-m_alpha)^(1.8/depth), which leaves a surface where it was and
     * thins everything denser in proportion.
     *
     * Off for the hypercube, whose look is already settled.
     */
    bool m_adapt = false;

    /** Faces along a line of sight, measured each frame. */
    T m_depth = 1.8;

    /** Projected face area this frame, summed over every object. */
    mutable T m_area = 0;

    /** Stereographic outermost step; see transform().  The h key. */
    bool m_stereo = false;

    /** Frustum offset for the steps after a stereographic one. */
    T m_eye_offset = 0;

    /**
     * Extent of the stereographic image, measured each frame.
     *
     * It has to be measured, and it has to be measured every frame.  The
     * perspective steps that follow assume a figure of about unit radius --
     * that is what the eye offset and the clip volume are sized for -- but
     * the stereographic image breathes as the figure turns, between about 1
     * and 5 at D=6, because how much it magnifies depends on how close the
     * figure passes to the pole.  Left unscaled, parts of it arrive within
     * 0.36 of the eye where the frustum's near plane is at 1.5, and dividing
     * by w then magnifies them by an order of magnitude relative to the far
     * side: the figure appears to sweep forward and swallow the camera.
     *
     * mutable because transform() is const, as with the framing radius.
     */
    mutable T m_stereo_scale = 1;

    /**
     * Average the normals of the faces meeting at a vertex.
     *
     * Right for a surface, wrong for a hypercube: its faces meet at right
     * angles, so averaging would round off corners that are not round.  Set
     * from the shape, and the m key overrides it to compare.
     */
    bool m_smooth = false;

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
                         const std::vector<T>& normal,
                         const std::vector<uint>&) {
    glBegin(GL_POLYGON);
    for(uint k = 0; k < corner.size(); k++) {
        glNormal3d(normal[3 * k], normal[3 * k + 1], normal[3 * k + 2]);
        glVertex4dv(corner[k].data());
    }
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

    m_area = 0;

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
        // How big the stereographic image comes out this frame, before
        // anything downstream assumes a size.  A separate pass because
        // transform() works a vertex at a time and this is a property of all
        // of them; the percentile is for the same reason the framing uses one,
        // that a vertex approaching the pole runs off on its own.
        if(m_stereo && math::Plot<T>::D > 3) {
            const int d = math::Plot<T>::D;

            std::vector<T> radii;
            radii.reserve(object.size());

            for(uint j = 0; j < object.size(); j++) {
                math::vertex<T> p(d);
                p = math::Plot<T>::modelview.top() * object[j]();

                T denom = 1 - p[d - 1];
                if(denom < 0.05) denom = 0.05;

                T r = 0;
                for(int i = 0; i < d - 1; i++) {
                    const T x = p[i] / denom;
                    r += x * x;
                }

                radii.push_back(r);
            }

            std::sort(radii.begin(), radii.end());

            const uint at = static_cast<uint>(radii.size() * 0.98);
            const T r = radii.empty()
                ? 1 : std::sqrt(radii[std::min<uint>(at, radii.size() - 1)]);

            m_stereo_scale = (r > 1e-9) ? r : 1;
        }

        std::vector< math::vertex<T> > transformed;
        transformed.reserve(object.size());

        for(uint j = 0; j < object.size(); j++) {
            transformed.push_back(transform(object[j]));
        }

        // Frame the object: far enough back that the outermost vertex sits
        // inside the field, growing the distance if a later rotation reaches
        // further than anything seen so far.
        // The outermost vertex, or near enough.
        //
        // Under stereographic projection a vertex approaching the pole runs
        // off toward infinity, and since the framing only ever grows, one of
        // them would shrink the figure to a dot and keep it there.  Taking a
        // high percentile instead lets a handful diverge without dragging the
        // rest with them; the divergence is still drawn, just not framed for.
        std::vector<T> radii;
        radii.reserve(transformed.size());

        for(uint j = 0; j < transformed.size(); j++) {
            const math::vertex<T>& v = transformed[j];
            T r2 = 0;
            for(uint k = 0; k < 3 && k < v.D; k++)
                r2 += v[k] * v[k];
            radii.push_back(r2);
        }

        std::sort(radii.begin(), radii.end());

        const uint at = m_stereo
            ? static_cast<uint>(radii.size() * 0.98)
            : (radii.empty() ? 0 : radii.size() - 1);

        T radius = radii.empty() ? 0 : std::sqrt(radii[std::min<uint>(at, radii.size() - 1)]);

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
            //
            // The normals are wanted here too rather than at draw time,
            // because a vertex normal is the average over the faces meeting
            // at it and that cannot be known one face at a time.
            std::vector< std::pair<T,uint> > order;
            order.reserve(faces.size());

            std::vector<T> fnormal(3 * faces.size(), 0);
            std::vector<bool> flat(faces.size(), false);

            std::vector< math::vertex<T> > fcorner;
            math::vertex<T> fn(3);

            for(uint f = 0; f < faces.size(); f++) {
                T z = 0;
                fcorner.clear();
                for(uint k = 0; k < faces[f].size(); k++) {
                    z += transformed[faces[f][k]][2];
                    fcorner.push_back(transformed[faces[f][k]]);
                }

                order.push_back(std::make_pair(z / faces[f].size(), f));

                if(face_normal(fcorner, fn)) {
                    flat[f] = true;
                    for(uint k = 0; k < 3; k++) fnormal[3 * f + k] = fn[k];
                }
            }

            // Vertex normals: the mean of the faces meeting at a vertex,
            // which is what makes a curved surface shade as curved rather
            // than as the flat quads it is made of.
            //
            // Aligned before averaging, and only where the shape is really a
            // surface.  A hypercube's faces meet at right angles and its
            // shadow everts, so averaging there would round off corners that
            // are not round and cancel to nothing wherever two faces face
            // opposite ways.  A torus is a genuine surface with consistent
            // winding, so its faces agree -- except along the fold curves,
            // where the projection reverses orientation.  Taking the first
            // face at each vertex as the reference and flipping the rest to
            // agree keeps the average meaningful across a fold; the fold
            // itself then reads as a crease, which is what it is.
            std::vector<T> vnormal;
            if(m_smooth) {
                vnormal.assign(3 * object.size(), 0);
                std::vector<bool> seen(object.size(), false);

                for(uint f = 0; f < faces.size(); f++) {
                    if(!flat[f]) continue;

                    for(uint k = 0; k < faces[f].size(); k++) {
                        const uint v = faces[f][k];

                        T dot = 0;
                        for(uint x = 0; x < 3; x++)
                            dot += vnormal[3 * v + x] * fnormal[3 * f + x];

                        const T sign = (seen[v] && dot < 0) ? -1.0 : 1.0;
                        seen[v] = true;

                        for(uint x = 0; x < 3; x++)
                            vnormal[3 * v + x] += sign * fnormal[3 * f + x];
                    }
                }

                for(uint v = 0; v < object.size(); v++) {
                    T len = 0;
                    for(uint x = 0; x < 3; x++)
                        len += vnormal[3 * v + x] * vnormal[3 * v + x];
                    len = std::sqrt(len);

                    // A vertex whose faces cancel has no mean direction; the
                    // face normal is used for it instead, at draw time.
                    if(len < 1e-12) { seen[v] = false; continue; }

                    for(uint x = 0; x < 3; x++) vnormal[3 * v + x] /= len;
                }

                for(uint v = 0; v < object.size(); v++)
                    if(!seen[v])
                        for(uint x = 0; x < 3; x++) vnormal[3 * v + x] = 0;
            }

            // How deeply the faces stack: total projected area over the area
            // they cover.  A face crossing a given line of sight contributes
            // to both, so the ratio is the mean number along one.  Scale-free,
            // so measuring before GL scales the figure is fine.
            if(m_adapt) {
                T area = 0;
                for(uint f = 0; f < faces.size(); f++) {
                    T a = 0;
                    for(uint k = 0; k < faces[f].size(); k++) {
                        const math::vertex<T>& p = transformed[faces[f][k]];
                        const math::vertex<T>& q =
                            transformed[faces[f][(k + 1) % faces[f].size()]];

                        a += p[0] * q[1] - q[0] * p[1];
                    }
                    area += std::fabs(a) / 2;
                }

                // Accumulated across every object rather than set from this
                // one.  Nested tori all lie along the same lines of sight, so
                // what matters for alpha is their total: four surfaces at 1.8
                // deep each stack 7 deep, and treating each as though it were
                // alone would let them accumulate to near opacity.  Read a
                // frame late, which is invisible at these rotation rates.
                m_area += area;
            }

            // ascending z: the camera looks down -z, so smallest is farthest
            std::sort(order.begin(), order.end());

            glShadeModel(GL_SMOOTH);
            glEnable(GL_LIGHTING);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_CULL_FACE);
            glDepthMask(GL_FALSE);

            std::vector< math::vertex<T> > corner;
            std::vector<uint> index;
            std::vector<T> normal;

            for(uint o = 0; o < order.size(); o++) {
                const uint f = order[o].second;
                if(!flat[f]) continue;

                const typename math::object<T>::face_type& face = faces[f];

                corner.clear();
                index.clear();
                normal.clear();

                for(uint k = 0; k < face.size(); k++) {
                    const uint v = face[k];

                    corner.push_back(transformed[v]);
                    index.push_back(base + v);

                    // Per corner where the surface has a mean direction there,
                    // and the face's own normal otherwise -- which covers both
                    // the flat-shaded shapes and the cancelled vertices.
                    T len = 0;
                    if(m_smooth)
                        for(uint x = 0; x < 3; x++)
                            len += vnormal[3 * v + x] * vnormal[3 * v + x];

                    if(len > 0) {
                        // to the side this face is facing, so a fold creases
                        // rather than shading through itself
                        T dot = 0;
                        for(uint x = 0; x < 3; x++)
                            dot += vnormal[3 * v + x] * fnormal[3 * f + x];

                        const T sign = (dot < 0) ? -1.0 : 1.0;
                        for(uint x = 0; x < 3; x++)
                            normal.push_back(sign * vnormal[3 * v + x]);
                    } else {
                        for(uint x = 0; x < 3; x++)
                            normal.push_back(fnormal[3 * f + x]);
                    }
                }

                draw_face(corner, normal, index);
            }

            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
            glDisable(GL_LIGHTING);
        }

        if(!m_wire) {
            base += object.size();
            continue;
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

    // The frame's total, for the next frame's alpha.
    if(m_adapt) {
        const T disc = PI * m_radius * m_radius;

        m_depth = (disc > 0 && m_area > disc) ? (m_area / disc) : 1.8;
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

    // Stereographic for the outermost step, when the shape allows it.
    //
    // Only from a sphere: the map sends a point of the unit sphere S^(d-1) to
    // the plane through its equator, along the line from the north pole,
    //
    //     x_i' = x_i / (1 - x_(d-1))
    //
    // which needs |x| = 1 to mean anything.  The torus is built on the unit
    // sphere and rotation preserves it, so it qualifies; a hypercube does not,
    // and its vertices sit at radius sqrt(n).
    //
    // The reason to want it is that stereographic projection sends circles to
    // circles.  The Clifford torus is ruled by the Hopf fibres, so at D=4 they
    // come through as Villarceau circles -- linked circles lying on an
    // ordinary donut -- which perspective projection does not show.
    //
    // Note the eye offset has to be zero for this to work at all: any
    // translation before the divide moves the figure off the sphere.  That is
    // why initialize_glazzies zeroes it in this mode and the offset for the
    // remaining perspective steps is applied afterwards, below, once the
    // sphere is no longer needed.
    int d = math::Plot<T>::D;

    if(m_stereo && d > 3) {
        const T pole = ret[d - 1];

        // Clamped: a point reaching the pole projects to infinity.  That
        // divergence is the eversion rather than a fault, so it is bounded
        // instead of dropped, and the framing uses a percentile so a few
        // vertices on their way out cannot carry the whole figure with them.
        T denom = 1 - pole;
        if(denom < 0.05) denom = 0.05;

        // Normalized to about unit radius, so the perspective steps that
        // follow get the size of figure their frustum was built for.  A
        // uniform scale, so it changes nothing about the shape.
        math::vertex<T> s(d - 1);
        for(int i = 0; i < d - 1; i++)
            s[i] = ret[i] / (denom * m_stereo_scale);

        // Now put it in front of the frustums the remaining steps use.  The
        // eye offset could not be applied before the divide without spoiling
        // the sphere, so it lands here instead.
        for(int i = 3; i < d - 1; i++)
            s[i] += m_eye_offset;

        ret = s;
        ret.change(d - 1);
        d--;
    }

    this->build_projections();

    for(; d > 3; d--) {
        // Reset the homogeneous coordinate, then project in place.
        //
        // This used to build a fresh vertex of the same dimensionality, copy
        // ret into it and multiply that.  The copy was not the point: vertex(d)
        // sets its own homogeneous coordinate to 1 and operator=(vertex) copies
        // only the spatial elements, so what the temporary did was reset w
        // before the multiply.  That matters in orthographic mode, where
        // normalize() is skipped and w is not already 1, so it is kept -- but
        // as one assignment rather than an allocation and a copy per step per
        // vertex.  change() runs unconditionally here, so ret's dimensionality
        // tracks the step and it can be projected in place.
        ret[d] = 1;
        ret = this->m_project[math::Plot<T>::D - d] * ret();

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

using apps::triple;
using apps::hsv;
using apps::hue_mean;


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
                           const std::vector<T>& normal,
                           const std::vector<uint>& index);

    void key_pressed(unsigned char key,int x,int y);

    /**
     * A hypercube is a solid whose shadow is a shadow; the flat torus is a
     * surface that cannot exist in three dimensions at all.  Both want the
     * same pipeline and show different things through it.
     */
    enum Shape { CUBOID, TORUS };
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
    Shape m_shape = CUBOID;

    /** Circles in the torus.  Only 2 makes a surface; see initialize(). */
    uint m_circles = 2;

    /** Tori drawn through the Hopf foliation.  The n and b keys. */
    uint m_shells = 1;

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
    bool surface = false;

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

    switch(m_shape) {
    case CUBOID: {
        cuboid<T> object(n);
        this->add(object);
        break;
    }
    case TORUS: {
        // Circles, not dimensions: k=2 is a surface whatever n is, so the
        // mesh does not grow when the figure turns in more planes.
        uint k = m_circles;
        if(k < 1) k = 1;
        if(2 * k > n) k = n / 2;
        if(k < 1) k = 1;

        // m^k vertices, so the samples per circle have to come down as
        // circles go up or the mesh explodes: 32 per circle is 1024 at k=2
        // and 32768 at k=3.  Holding m^k near 1024 gives 32, 10, 6, 4.
        uint m = static_cast<uint>(std::lround(std::pow(1024.0, 1.0 / k)));
        if(m < 3) m = 3;
        if(m > 64) m = 64;

        // One torus, or a stack of them through the Hopf foliation.
        //
        // Radii (cos a, sin a) give a torus for every a in (0, pi/2): they are
        // disjoint, they fill the sphere apart from the two circles they close
        // down onto at the ends, and each threads through the last.  Spacing a
        // evenly across the interval samples that family.  a = pi/4 is the
        // Clifford torus, which is what a single shell gives.
        //
        // Only for k=2.  Above that the radii are a simplex rather than one
        // angle, and there is no single family to walk.
        const uint shells = (k == 2 ? m_shells : 1);

        hues.clear();

        for(uint shell = 0; shell < shells; shell++) {
            std::vector<T> weight;

            if(shells > 1) {
                const T a = PI / 2 * (shell + 1) / (shells + 1);

                weight.push_back(std::cos(a));
                weight.push_back(std::sin(a));
            }

            torus<T> object(n, k, m, weight);
            this->add(object);

            // One hue per shell, so the nesting is legible.  Within a shell
            // the shading is the surface's own; between them it is the colour
            // that says which is which where they pass through each other.
            const T hue = (shells > 1)
                ? static_cast<T>(shell) / shells
                : 0.0;

            for(uint i = 0; i < object.size(); i++)
                hues.push_back(shells > 1
                               ? hue
                               : static_cast<T>(i % object.M) / object.M);

            if(shell == 0)
                std::cout << "torus: " << k << " circles, " << m
                          << " per circle, " << shells << " shell"
                          << (shells > 1 ? "s, " : ", ")
                          << shells * object.size() << " vertices, "
                          << shells * object.get_faces().size() << " faces"
                          << std::endl;
        }

        // A single torus is banded by position around its first circle:
        // scattered hues are right when vertices are landmarks, but a mesh
        // vertex is just where the surface got sampled, and scattering across
        // 1024 of them gives every face four unrelated corners to blend and
        // renders the surface as coloured static.  The wrap is exact and
        // free, position around a circle being cyclic like hue itself.
        surface = (k == 2);
        break;
    }
    }

    // Only a 2-torus is a surface.  Above that the faces are interior
    // scaffold: there is nothing for smooth normals to smooth, the edges are
    // structure again rather than a mesh laid over something, and the faces
    // stack deeply enough that the alpha has to be measured rather than set.
    this->m_smooth = surface;
    this->m_wire   = !surface;
    this->m_alpha  = (m_shape == TORUS ? 0.5 : 0.18);
    this->m_adapt  = (m_shape == TORUS);
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
                                  const std::vector<T>& normal,
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
    // Thinned by how deeply the faces stack, when the shape needs it.
    const T alpha = this->m_adapt
        ? 1 - std::pow(1 - this->m_alpha, 1.8 / this->m_depth)
        : this->m_alpha;

    fcolors[3] = static_cast<GLfloat>(alpha);

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
    } else if(key == 'n' || key == 'b') {
        const uint want = (key == 'n' ? m_shells + 1 : m_shells - 1);

        if(want >= 1 && want <= 8) {
            m_shells = want;
            if(m_shape == TORUS) initialize(this->D);
        }
    } else if(key == 'h') {
        // Only from the sphere; a hypercube is not on one.
        if(m_shape == TORUS) {
            this->m_stereo = !this->m_stereo;
            this->reframe();
            initialize(this->D);

            std::cout << "projection: "
                      << (this->m_stereo ? "stereographic" : "perspective")
                      << std::endl;
        }
    } else if(key == 'k' || key == 'j') {
        const uint want = (key == 'k' ? m_circles + 1 : m_circles - 1);

        if(want >= 1 && 2 * want <= this->D) {
            m_circles = want;
            if(m_shape == TORUS) initialize(this->D);
        }
    } else if(key == 'l') {
        this->m_wire = !this->m_wire;
        this->draw();
    } else if(key == 'm') {
        this->m_smooth = !this->m_smooth;
        this->draw();
    } else if(key == 't') {
        m_shape = (m_shape == CUBOID ? TORUS : CUBOID);
        initialize(this->D);
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

        // Zero under stereographic projection: it needs the figure on the
        // unit sphere, and a translation would take it off.  transform()
        // applies the offset after the divide instead, for whatever
        // perspective steps remain.
        eye[i] = (this->m_stereo ? 0 : r2);
        up[i] = 0;
        center[i] = 0;
    }

    this->m_eye_offset = r2;
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

