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

#include <jlib/glu/textures.hh>
#include <jlib/gl/opengl.hh>
#include <jlib/gl/opengl.hh>

const int RES     = 128;
const int DEPTH   = 4;

namespace jlib {
namespace glu {
namespace textures {
        
void init(const std::string& data) {
    GLuint texture;

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

std::string make_checker2d() {
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
    
}
}
}
