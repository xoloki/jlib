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

#ifndef JLIB_GLUT_MAIN_HH
#define JLIB_GLUT_MAIN_HH

#include <jlib/sys/signal.hh>
#include <string>

namespace jlib {
namespace glut {

class Main {
public:
    static void init(int argc, char** argv, 
                     bool is_fullscreen=true, 
                     bool is_double=true, 
                     bool is_depth=true);
    
    static void run();
    
    static sys::signal<void()> init_buffers;
    static sys::signal<void()> init_lights;
    static sys::signal<void()> init_textures;
    
    static sys::signal<std::string()> make_textures;
    
    static sys::signal<void()> display;
    static sys::signal<void(int,int)> reshape;
    static sys::signal<void(unsigned char,int,int)> keyboard;
    static sys::signal<void(int,int,int)> special;
    static sys::signal<void(int,int,int,int)> mouse;
    static sys::signal<void()> idle;
    
    static void on_display();
    static void on_reshape(int,int);
    static void on_keyboard(unsigned char,int,int);
    static void on_special(int,int,int);
    static void on_mouse(int,int,int,int);
    static void on_idle();
    
    static void default_reshape(int,int);
    
    static void default_init_buffers();
    static void default_init_lights();
            static void default_init_textures();
    
    static std::string make_checkered_texture();
    
    static void connect_defaults();
    
    static void transparent_init_buffers();
    static void transparent_init_lights();
    
    static bool is_fullscreen();
    static bool is_double();
    static bool is_depth();
    
    static int get_width();
    static int get_height();
    
    static void set_width(int s);
    static void set_height(int s);
    
protected:
    static bool m_is_fullscreen;
    static bool m_is_double;
    static bool m_is_depth;
    static int m_width;
    static int m_height;
};
    
}
}

#endif
