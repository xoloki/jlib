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


#include <jlib/math/math.hh>
#include <jlib/glx/Window.hh>
#include <jlib/gl/lights.hh>
#include <jlib/gl/buffers.hh>
#include <jlib/glu/textures.hh>

#include <iostream>

#include <cstdlib>

#include <GL/glu.h>
#include <GL/glut.h>

#include <unistd.h>

using namespace jlib;

const GLdouble PI = 3.14159265358979323846264338;
const GLdouble PI2 = 2 * 3.14159265358979323846264338;

GLdouble eyex = 0;
GLdouble eyey = 0;
GLdouble eyez = 3.5;

GLdouble centerx = 0;
GLdouble centery = 0;
GLdouble centerz = 0;

GLdouble upx = 0;
GLdouble upy = 1;
GLdouble upz = 0;

GLdouble outer_rad = 0;
GLdouble inner_x_rad = 0;
GLdouble inner_y_rad = 0;
GLdouble inner_z_rad = 0;
GLdouble bounce_rad = 0;
GLdouble color_rad = 0;

GLdouble outer_inc = 0.005;
GLdouble inner_x_inc = 0.01;
GLdouble inner_y_inc = 0.02;
GLdouble inner_z_inc = 0.03;
GLdouble bounce_inc = 0.005;
GLdouble color_inc = 0.005;

GLdouble inner = 0.17;
GLdouble outer = 0.40;
GLint sides = 55;
GLint rings = 55;

int N = 6;
const GLdouble H = 2.3;

bool paused = false;

void on_keyboard(std::string key, int x, int y);
void on_mouse(int button, int x, int y);
void on_idle(glx::Window& window);
void increment(GLdouble& rad, const GLdouble& inc);
void make_color(GLfloat* vals, int i);
void display(glx::Window& window);

int main(int argc, char** argv) {
    try {
        glx::Window window("GLX Box", 500, 500);

        window.key_press.connect(sigc::ptr_fun(&on_keyboard));
        window.button_press.connect(sigc::ptr_fun(&on_mouse));
        window.timeout.connect(sigc::bind(sigc::ptr_fun(&on_idle), window));

        gl::buffers::init(false, true, true);
        gl::lights::init(false);
        glu::textures::init(glu::textures::make_checker2d());

        window.run();
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        std::exit(1);
    } catch(...) {
        std::cerr << "unknown exception" << std::endl;
        std::exit(1);
    }

    std::exit(0);
}

void display(glx::Window& window) {
    GLUquadric* quad = gluNewQuadric();
    
    window.clear();

	glLoadIdentity();
	gluLookAt(eyex, eyey, eyez, centerx, centery, centerz, upx, upy, upz);

    for(int i = 0; i < N; i++) {
        GLdouble h = H * std::sin(bounce_rad);
        GLdouble rad = outer_rad + PI2 * (static_cast<double>(i) / static_cast<double>(N));
        GLdouble x = h * std::cos(rad);
        GLdouble y = h * std::sin(rad);
        GLfloat vals[4];

        glPushMatrix();

        make_color(vals, i);
        glColor4fv(vals);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, vals);

        glTranslatef(x, y, 0);

        glRotatef(inner_x_rad * 180 / PI, 1, 0, 0);
        glRotatef(inner_y_rad * 180 / PI, 0, 1, 0);
        glRotatef(inner_z_rad * 180 / PI, 0, 0, 1);

        gluSphere(quad, outer, sides, rings);

        glPopMatrix();
    }

    window.flush();
    gluDeleteQuadric(quad);
}

void make_color(GLfloat* vals, int i) {
    GLdouble rad = color_rad + PI2 * (static_cast<double>(i) / static_cast<double>(N));

    vals[0] = 0.5 + std::cos(rad)/2.0;
    vals[1] = 0.5 + std::cos(rad + PI/2)/2.0;
    vals[2] = 0.5 + std::cos(rad + PI)/2.0;
    vals[3] = 0.7;
}

void on_mouse(int button, int x, int y) {
    
}

void on_keyboard(std::string key,int x,int y) {
    if(key == "q") {
        std::exit(0);
    } else if(key == " ") {
        paused = !paused;
    }
}

void on_idle(glx::Window& window) {
    if(!paused) {
        increment(outer_rad, outer_inc);
        increment(inner_x_rad, inner_x_inc);
        increment(inner_y_rad, inner_y_inc);
        increment(inner_z_rad, inner_z_inc);
        increment(bounce_rad, bounce_inc);
        increment(color_rad, color_inc);
    }

    display(window);
}

void increment(GLdouble& rad, const GLdouble& inc) {
    rad += inc;
    if(rad > PI2) {
        rad = std::fmod(rad, PI2);
    }
}
