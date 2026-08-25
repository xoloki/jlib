/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_GLFW_PLOT_HH
#define JLIB_GLFW_PLOT_HH

#include <jlib/glfw/Window.hh>

#include <jlib/gl/buffers.hh>
#include <jlib/gl/lights.hh>
#include <jlib/glu/projection.hh>

#include <jlib/math/Plot.hh>

#include <vector>

namespace jlib {
namespace glfw {

/**
 * A math::Plot backend on a GLFW window.
 *
 * Shaped like glx::Plot -- multiply inherited from the plot and its window --
 * rather than like glut::Plot, which owned no window because glut::Main had
 * created one behind its back.
 *
 * Note the constructor sets GL state directly rather than emitting a signal
 * for someone else to have connected to.  glut::Main::init emitted
 * init_buffers and init_lights *before* glut::Plot's constructor connected to
 * them, so GL_DEPTH_TEST was never once enabled in any of the hyper apps --
 * an ordering hazard that goes away when there is no signal in the path.
 */
template<typename T>
class Plot : public math::Plot<T>, public Window {
public:
    Plot(uint n, std::vector< std::pair<T,T> > c, uint w = 400, uint h = 400,
         const std::string& title = "jlib::glfw::Plot");

    virtual void draw();
    virtual void draw_point(std::pair<uint,uint> p, uint index);
    virtual void draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2,
                           uint i1, uint i2);

protected:
    /**
     * Virtual so a subclass can choose its projection.  ortho2d is right for
     * backends that draw in pixel coordinates, which is what math::Plot hands
     * them today; a subclass reducing only as far as 3-D wants perspective
     * instead, or its geometry -- which sits around the origin at unit scale
     * -- collapses into a single pixel.
     */
    virtual void on_configure(int width, int height);
};


template<typename T>
inline
Plot<T>::Plot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h, const std::string& title)
    : math::Plot<T>(n, c, w, h),
      Window(title, w, h, true)
{
    // transparent=false so this takes the depth-test branch; the transparent
    // one disables depth and masks writes, which is for translucent solids.
    // No texturing: the plots draw lines and points.
    gl::buffers::init(false, true, false);

    // Deliberately NOT gl::lights::init().
    //
    // These plots are wireframe -- GL_LINES and GL_POINTS with no normals, so
    // every vertex would carry the default (0,0,1).  Enabling GL_LIGHTING
    // under those conditions does not light anything, it just makes glColor
    // stop working: with lighting on, colour comes from glMaterial instead,
    // and the whole figure washes to a flat shade.
    //
    // GLUT appeared to light these, but only because glut::Main emitted
    // init_lights before anything had connected to it, so lighting was never
    // actually on and glColor drove the colour.  Wireframe wants exactly
    // that.  Lighting belongs with faces and real normals -- see the solid
    // rendering work.

    // Window delivers a synthetic configure before the first frame, which is
    // what sizes the projection.  Calling on_configure() from here instead
    // would dispatch to this class rather than to a subclass's override,
    // since the subclass is not constructed yet.
    configure_notify.connect([this](auto&&... a) { return this->on_configure(a...); });
}


template<typename T>
inline
void Plot<T>::draw_point(std::pair<uint,uint> p, uint) {
    glBegin(GL_POINTS);
    glVertex2i(p.first, p.second);
    glEnd();
}


template<typename T>
inline
void Plot<T>::draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2,
                        uint, uint) {
    glBegin(GL_LINES);
    glVertex2i(p1.first, p1.second);
    glVertex2i(p2.first, p2.second);
    glEnd();
}


template<typename T>
inline
void Plot<T>::draw() {
    clear();

    math::Plot<T>::draw();

    flush();
}


template<typename T>
inline
void Plot<T>::on_configure(int width, int height) {
    this->width = width;
    this->height = height;

    // Framebuffer pixels, which is what Window::get_width/height report and
    // what glViewport wants.
    glViewport(0, 0, width, height);

    glu::projection::ortho2d(width, height);

    draw();
}

}
}

#endif //JLIB_GLFW_PLOT_HH
