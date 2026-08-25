/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2008 Joey Yandle <xoloki@gmail.com>
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

#include <jlib/glu/projection.hh>
#include <jlib/gl/opengl.hh>
#include <jlib/gl/opengl.hh>

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
