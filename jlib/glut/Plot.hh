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

#ifndef JLIB_GLUT_PLOT_HH
#define JLIB_GLUT_PLOT_HH

#include <jlib/math/math.hh>
#include <jlib/math/Plot.hh>
#include <jlib/glut/main.hh>
#include <GL/glut.h>

#include <vector>
#include <stack>


namespace jlib {
namespace glut {

        
template<typename T>
class Plot : public math::Plot<T> {
public:
    Plot(uint n, std::vector< std::pair<T,T> > c, uint w=400, uint h=400);

    virtual void draw();
    virtual void draw_point(std::pair<uint,uint> p);
    virtual void draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2);

protected:
    void on_reshape(int width, int height);
    void on_display();
    void on_idle();

    int m_win;
};


template<typename T>
inline
Plot<T>::Plot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
    : math::Plot<T>(n, c, w, h)
{    
    //m_win = glutCreateWindow("jlib::glut::Plot");
    //glutReshapeWindow(w, h);
    
    glut::Main::init_buffers.connect(sigc::ptr_fun(&glut::Main::default_init_buffers));
    glut::Main::init_lights.connect(sigc::ptr_fun(&glut::Main::default_init_lights));
    glut::Main::init_textures.connect(sigc::ptr_fun(&glut::Main::default_init_textures));
    //glut::Main::make_textures.connect(sigc::ptr_fun(&glut::Main::make_checkered_texture));
    
    glut::Main::reshape.connect(sigc::mem_fun(this, &Plot<T>::on_reshape));
    glut::Main::display.connect(sigc::mem_fun(this, &Plot<T>::on_display));
    glut::Main::idle.connect(sigc::mem_fun(this, &Plot<T>::on_idle));
}


template<typename T>
inline
void Plot<T>::draw_point(std::pair<uint,uint> p) {
    glBegin(GL_POINTS);
    glVertex2i(p.first, p.second);
    glEnd();
}


template<typename T>
inline
void Plot<T>::draw_line(std::pair<uint,uint> p1, std::pair<uint,uint> p2) {
    //Window::draw_line(p1, p2);
    glBegin(GL_LINES);
    glVertex2i(p1.first, p1.second);
    glVertex2i(p2.first, p2.second);
    glEnd();
}


template<typename T>
void
inline Plot<T>::draw() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    math::Plot<T>::draw();

	glutSwapBuffers();    
}

template<typename T>
void
inline Plot<T>::on_reshape(int width, int height) {
    this->width = width;
    this->height = height;

    glut::Main::default_reshape(width, height);

	glLoadIdentity();
	gluLookAt(0, 0, 1, 0, 0, 0, 0, 1, 0);

    glutPostRedisplay();
}

template<typename T>
void
inline Plot<T>::on_display() {
    draw();
}

template<typename T>
void
inline Plot<T>::on_idle() {
    glutPostRedisplay();
}

}
}

#endif
