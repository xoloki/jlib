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

