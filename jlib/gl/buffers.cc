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

#include <jlib/gl/buffers.hh>
#include <iostream>

namespace jlib {
namespace gl {
namespace buffers {

void init(bool transparent, bool depth, bool texture) {
    glClearColor( 0.0, 0.0, 0.0, 0.0 );

    if(transparent) {
        glDisable(GL_DEPTH_TEST);
        glShadeModel(GL_SMOOTH);
        glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA,GL_ONE);
    } else {
        if(depth) {
            glShadeModel(GL_SMOOTH);
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);
            glEnable(GL_AUTO_NORMAL);
            glEnable(GL_POLYGON_SMOOTH);
        }
        else {
            glShadeModel(GL_FLAT);
        }
    }

    if(texture) {
        glEnable(GL_TEXTURE_2D);
    }
}


}
}
}

