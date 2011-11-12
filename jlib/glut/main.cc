/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 2008 Joe Yandle <jwy@divisionbyzero.com>
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

#include <jlib/glut/main.hh>

#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glut.h>

const int RES     = 128;
const int DEPTH   = 4;

namespace jlib {
namespace glut {

void Main::init(int argc, char** argv, bool is_fullscreen, bool is_double, bool is_depth) {
    m_is_fullscreen = is_fullscreen;
    m_is_double = is_double;
    m_is_depth = is_depth;
    
    glutInit( &argc, argv );
    unsigned int mode = GLUT_RGBA;
    if(is_double)
        mode |= GLUT_DOUBLE;
    else
        mode |= GLUT_SINGLE;
    
    if(is_depth)
        mode |= GLUT_DEPTH;
    
    glutInitDisplayMode(mode);
    glutInitWindowSize(m_width,m_height);
    glutInitWindowPosition(0,0);
    glutCreateWindow(argv[0]);
    
    init_buffers.emit();
    init_lights.emit();
    init_textures.emit();
    
    glutDisplayFunc(on_display);
    glutReshapeFunc(on_reshape);
    glutKeyboardFunc(on_keyboard);
    glutSpecialFunc(on_special);
    glutMouseFunc(on_mouse);
    glutIdleFunc(on_idle);
    glutSetCursor(GLUT_CURSOR_NONE);
    
    if(is_fullscreen)
        glutFullScreen();
}
    
void Main::run() {
    glutMainLoop();
}
    
sigc::signal0<void> Main::init_buffers;
sigc::signal0<void> Main::init_lights;
sigc::signal0<void> Main::init_textures;
    
sigc::signal0<std::string> Main::make_textures;
    
sigc::signal0<void> Main::display;
sigc::signal2<void,int,int> Main::reshape;
sigc::signal3<void,unsigned char,int,int> Main::keyboard;
sigc::signal3<void,int,int,int> Main::special;
sigc::signal4<void,int,int,int,int> Main::mouse;
sigc::signal0<void> Main::idle;
    
    
    
void Main::on_display() { display.emit(); }
void Main::on_reshape(int w, int h) { reshape.emit(w,h); }
void Main::on_keyboard(unsigned char key, int x, int y) { keyboard.emit(key,x,y); }
void Main::on_special(int key, int x, int y) { special.emit(key, x, y); }
void Main::on_mouse(int button, int state, int x, int y) { mouse.emit(button,state,x,y); }
void Main::on_idle() { idle.emit(); }
    
void Main::default_reshape(int w, int h) {
    m_width = w;
    m_height = h;
    
    glViewport( 0, 0, (GLsizei) w, (GLsizei) h );
    glMatrixMode( GL_PROJECTION );
    glLoadIdentity();
    if(is_depth())
        gluPerspective( 80.0, ( GLfloat ) w / ( GLfloat ) h, 0.1, 50 );
    else
        gluOrtho2D(0, (GLdouble)w, 0, (GLdouble)h);
    
    glMatrixMode( GL_MODELVIEW );
}
    
void Main::default_init_buffers() {
    glClearColor( 0.0, 0.0, 0.0, 0.0 );
    if(is_depth()) {
        glShadeModel(GL_SMOOTH);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glEnable(GL_AUTO_NORMAL);
        glEnable(GL_POLYGON_SMOOTH);
    }
    else {
        glShadeModel(GL_FLAT);
    }
    
    glEnable(GL_TEXTURE_2D);
}
    
void Main::transparent_init_buffers() {
    glClearColor( 0.0, 0.0, 0.0, 0.0 );
    
    glDisable(GL_DEPTH_TEST);
    glShadeModel(GL_SMOOTH);
    glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE);
    //glEnable(GL_AUTO_NORMAL);
    //glEnable(GL_POLYGON_SMOOTH);
    
    glEnable(GL_TEXTURE_2D);
}
    
void Main::default_init_lights() {
    GLfloat amb[] = { 0.2, 0.2, 0.2, 2.0 };
    GLfloat pos[] = { 0.0, 0.0, 1.0, 0.0 };
    GLfloat spe[] = { 1.0, 1.0, 1.0, 1.0 };
    GLfloat shi[] = { 100.0 };
    
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    
    glLightfv(GL_LIGHT0, GL_AMBIENT, amb);
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    
    glMaterialfv(GL_FRONT, GL_SPECULAR, spe);
    glMaterialfv(GL_FRONT, GL_SHININESS, shi);
}
    
void Main::transparent_init_lights() {
    GLfloat amb[] = { 0.0, 0.0, 0.0, 1.0 };
    GLfloat dif[] = { 1.0, 1.0, 1.0, 1.0 };
    GLfloat pos[] = { 1.0, 1.0, 1.0, 0.0 };
    GLfloat spe[] = { 1.0, 1.0, 1.0, 1.0 };
    GLfloat shi[] = { 50.0 };
    
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    
    glLightfv(GL_LIGHT0, GL_AMBIENT, amb);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
    glLightfv(GL_LIGHT0, GL_SPECULAR, spe);
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spe);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SHININESS, shi);
}
    
void Main::default_init_textures() {
    GLuint texture;
    std::string data = make_textures.emit();
    
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,RES,RES,0,GL_RGBA,GL_UNSIGNED_BYTE,data.data());
    gluBuild2DMipmaps(GL_TEXTURE_2D,GL_RGB,RES,RES,GL_RGBA,GL_UNSIGNED_BYTE,data.data());
        
    glEnable(GL_TEXTURE_GEN_S);
    glEnable(GL_TEXTURE_GEN_T);
    
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
    glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
}
    
std::string Main::make_checkered_texture() {
    std::string data(RES*RES*DEPTH, 0);
    int c;
    int dv = 0x08;
    int pix = 0x40 / dv;
    
    for(int i=0; i<RES; i++) {
        for(int j=0; j<RES; j++) {
            c = 255 * ( ( ( i & pix ) == 0 ) ^ ( ( j & pix ) == 0 ) );
            for(int k=0; k<DEPTH-1; k++)
                data[i*RES*DEPTH + j*DEPTH + k] = c;
            data[i*RES*DEPTH + j*DEPTH + (DEPTH-1)] = 255;
        }
    }
        
    return data;
}
    
void Main::connect_defaults() {
    reshape.connect(sigc::ptr_fun(&default_reshape));
    
    init_buffers.connect(sigc::ptr_fun(&default_init_buffers));
    init_lights.connect(sigc::ptr_fun(&default_init_lights));
    init_textures.connect(sigc::ptr_fun(&default_init_textures));
    
    make_textures.connect(sigc::ptr_fun(&make_checkered_texture));
}
    
bool Main::is_fullscreen() { return m_is_fullscreen; }
bool Main::is_double() { return m_is_double; }
bool Main::is_depth() { return m_is_depth; }
    
bool Main::m_is_fullscreen=true;
bool Main::m_is_double=true;
bool Main::m_is_depth=true;
    
int Main::m_width=300;
int Main::m_height=300;
    
int Main::get_height() { return m_height; }
int Main::get_width() { return m_width; }
void Main::set_height(int s) { m_height=s; }
void Main::set_width(int s) { m_width=s; }
    
}
}
