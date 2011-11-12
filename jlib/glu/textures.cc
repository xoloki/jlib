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

#include <jlib/glu/textures.hh>
#include <GL/glu.h>
#include <GL/gl.h>

const int RES     = 128;
const int DEPTH   = 4;

namespace jlib {
namespace glu {
namespace textures {
        
void init(std::string data) {
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
