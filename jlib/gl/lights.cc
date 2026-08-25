/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2011 Joey Yandle <xoloki@gmail.com>
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
 */

#include <jlib/gl/lights.hh>
#include <iostream>

namespace jlib {
namespace gl {
namespace lights {

void init(bool transparent) {
    if(transparent) {
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

    } else {
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
}

    
}
}
}
