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

#include <iostream>

#include <cstdlib>

#include <unistd.h>

#include <glibmm/thread.h>
#include <glibmm/timer.h>

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
//     glBegin(GL_POINTS);
//     glVertex2i(p.first, p.second);
//     glEnd();
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
	//gluLookAt(0, 0, 3, 0, 0, 0, 0, 1, 0);

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

using namespace jlib;
using namespace jlib::math;

typedef GLdouble T;




template<typename T>
class HPlot : public glut::Plot<T> {
public:
    HPlot(uint n, std::vector< std::pair<T,T> > c, uint w=400, uint h=400);

    virtual void draw();

    virtual void draw_point(math::vertex<T> p);
    virtual void draw_line(math::vertex<T> p1, math::vertex<T> p2);

protected:
    virtual math::vertex<T> transform(const math::vertex<T>& v) const;

};

template<typename T>
inline
HPlot<T>::HPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
    : glut::Plot<T>(n, c, w, h)
{    

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

	glutSwapBuffers();    
}

template<typename T>
inline
math::vertex<T> HPlot<T>::transform(const math::vertex<T>& vertex) const {
    math::vertex<T> ret(math::Plot<T>::D);

    ret = math::Plot<T>::modelview.top() * vertex();
    //ret = vertex;
    //ret.normalize();
    //ret.change(ret.D - 1);

    // keep projecting until we get to two dimensions
    for(int d = math::Plot<T>::D; d > 3; d--) {
        math::matrix<T> p = math::matrix<T>::project(d, math::Plot<T>::clip);
        math::vertex<T> v(d);
        v = ret;
        ret = p * v();
        ret.normalize();
        ret.change(d-1);
    }

    return ret;
}

typedef HPlot<T> PlotType;

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
      r(n - 1),
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
    r = n - 1;

    initialize_glazzies(n);
    setClip(clip);
    change(n);
    
    initialize_rotation(n);
    (*this) * matrix<T>::lookAt(n, eye, up, center);

    cuboid<T> object(n);
    add(object);
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

    if(p1[p1.D] != 1)
        std::cout << "draw_line: p1[p1.D] = " << p1[p1.D] << std::endl;
    if(p2[p2.D] != 1)
        std::cout << "draw_line: p2[p2.D] = " << p2[p2.D] << std::endl;

    math::vertex<T> mid(p2.D);
    std::cout << std::fixed;
    for(unsigned int i = 0; i < mid.D; i++) {
        mid[i] = (p1[i] + p2[i]) / 2;
        std::cout << std::setw(5) << std::setprecision(2) << p1[i] << " "  << std::setw(5) << std::setprecision(2)<< p2[i] << " "  << std::setw(5) << std::setprecision(2)<< mid[i] << std::endl;
    } 
    std::cout << std::endl;
   
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
    clip.clear();
    clip.push_back(std::make_pair(-3, 3));
    clip.push_back(std::make_pair(-3, 3));
    
    eye.change(n);
    eye[0] = 0;
    eye[1] = 0;

    up.change(n);
    up[0] = 0;
    up[1] = 1;

    center.change(n);
    center[0] = 0;
    center[1] = 0;

    for(uint i = 2; i < n; i++) {
        clip.push_back(std::make_pair(-3, 3));
        i == 2 ? eye[i] = 3 : eye[i] = 0;
        //eye[i] = 2.5;
        up[i] = 0;
        center[i] = 0;
    }
}


class HardPlot : public HyperPlot<T, PlotType> {
public:
    HardPlot(uint n, std::vector< std::pair<T,T> > c, uint w, uint h) 
        : HyperPlot<T, PlotType>(n, c, w, h)
    {
        glut::Main::keyboard.connect(sigc::mem_fun(this, &HyperPlot<T,PlotType>::key_pressed));
        glut::Main::mouse.connect(sigc::mem_fun(this, &HyperPlot<T,PlotType>::button_pressed));
        glut::Main::idle.clear();
        glut::Main::idle.connect(sigc::mem_fun(this, &HardPlot::on_timeout));
    }

    void on_timeout() {
        HyperPlot<T, PlotType>::on_timeout();

        

        if(waiting)
            Glib::usleep(100000);
        else
            PlotType::on_idle();
    }
};

int main(int argc, char** argv) {
    uint D = 5;
    if(argc > 1) {
        D = atoi(argv[1]);
    }

    try {
        glut::Main::init(argc, argv, true, true, true);

        HardPlot plot(D, std::vector< std::pair<T,T> >(), 700, 700);
        
        glut::Main::run();
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

