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

#include <jlib/glu/projection.hh>
#include <GL/glu.h>
#include <GL/gl.h>

namespace jlib {
namespace glu {
namespace projection {

void perspective(int w, int h) {
    glViewport( 0, 0, (GLsizei) w, (GLsizei) h );
    glMatrixMode( GL_PROJECTION );
    glLoadIdentity();
    gluPerspective( 80.0, ( GLfloat ) w / ( GLfloat ) h, 0.1, 50 );
    glMatrixMode( GL_MODELVIEW );
}

void ortho2d(int w, int h) {
    glViewport( 0, 0, (GLsizei) w, (GLsizei) h );
    glMatrixMode( GL_PROJECTION );
    glLoadIdentity();
    gluOrtho2D(0, (GLdouble)w, 0, (GLdouble)h);
    glMatrixMode( GL_MODELVIEW );
}

}
}
}
