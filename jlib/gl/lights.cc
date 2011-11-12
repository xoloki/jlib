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
