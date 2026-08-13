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

    virtual void draw_point(math::vertex<T> p);
    virtual void draw_line(math::vertex<T> p1, math::vertex<T> p2);

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
    // This one is where solid rendering will go.
    this->set_projection_mode(math::Plot<T>::projection_mode::perspective);
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
void HPlot<T>::draw_point(math::vertex<T> p) {
    glBegin(GL_POINTS);
    //glVertex3d(p[0], p[1], p[2]);
    glVertex4dv(p.data());
    glEnd();
}

template<typename T>
inline
void HPlot<T>::draw_line(math::vertex<T> p1, math::vertex<T> p2) {
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
void HPlot<T>::draw() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    typename std::list< math::object<T> >::iterator i = math::Plot<T>::objects.begin();
    for(; i != math::Plot<T>::objects.end(); i++) {
        std::map<math::vertex<T>, math::vertex<T> > transformed;
        math::object<T>& object = *i;

        for(uint j = 0; j < object.size(); j++) {
            math::vertex<T>& v1 = object[j];
            math::vertex<T> tv1 = transform(v1);

            transformed.insert(std::make_pair(v1, tv1));
        }

        // Frame the object: far enough back that the outermost vertex sits
        // inside the field, growing the distance if a later rotation reaches
        // further than anything seen so far.
        T radius = 0;
        typename std::map<math::vertex<T>, math::vertex<T> >::iterator t;
        for(t = transformed.begin(); t != transformed.end(); t++) {
            const math::vertex<T>& v = t->second;
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

        // 80 degree vertical field, so a half-angle of 40; 0.6 leaves room for
        // the corners, which swing wider than the radius on any one frame.
        const T half_field = std::tan(40.0 * PI / 180.0);
        m_camera = 1.0 / (0.6 * half_field);

        // The 3-D camera, set here rather than through the N-D lookAt so it
        // is not scaled by the perspective divide.  See initialize_glazzies.
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        gluLookAt(0, 0, m_camera,
                  0, 0, 0,
                  0, 1, 0);
        glScaled(scale, scale, scale);

        for(uint j = 0; j < object.size(); j++) {
            math::vertex<T>& v1 = object[j];
            math::vertex<T>& tv1 = transformed.find(v1)->second;

            draw_point(tv1);
            
            std::list< math::vertex<T> > adjacent = object.adjacent(j);
            typename std::list< math::vertex<T> >::iterator k;
            for(k = adjacent.begin(); k != adjacent.end(); k++) {
                math::vertex<T>& v2 = *k;
                math::vertex<T>& tv2 = transformed.find(v2)->second;

                draw_line(tv1, tv2);
            }
        }
    }

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

template<typename T, typename Plot>
class HyperPlot : public Plot {
public:
    HyperPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h);

    virtual void change(uint n);
    virtual void draw();
    virtual void draw_point(math::vertex<T> point);
    virtual void draw_line(math::vertex<T> p1, math::vertex<T> p2);

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

};

template<typename T, typename Plot>
inline
HyperPlot<T,Plot>::HyperPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
    : Plot(n, c, w, h),
      r(n),
      first(true),
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
void HyperPlot<T,Plot>::draw_point(math::vertex<T> point) {
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
void HyperPlot<T,Plot>::draw_line(math::vertex<T> p1, math::vertex<T> p2) {
    triple<T> color = colors[i-1];
    //set_foreground(color.r, color.g, color.b);
    GLfloat fcolors[4];
    fcolors[0] = color.r; fcolors[1] = color.g; fcolors[2] = color.b; fcolors[3] = 1.0;
    glColor3fv(fcolors);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, fcolors);

    // This used to print every coordinate of every vertex on every frame,
    // which made the app unusably slow.  JLIB_PLOT_DUMP captures one frame to
    // a file instead; see jlib/math/dump.hh.
    math::vertex<T> mid(p2.D);
    for(unsigned int i = 0; i < mid.D; i++) {
        mid[i] = (p1[i] + p2[i]) / 2;
    }

    Plot::draw_line(p1, mid);
    //Plot::draw_line(p1, p2);
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

