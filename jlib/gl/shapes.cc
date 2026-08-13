/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2000 Joe Yandle <jwy@divisionbyzero.com>
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

#include <jlib/gl/shapes.hh>
#include <jlib/gl/opengl.hh>

#include <cmath>

namespace jlib {
namespace gl {
namespace shapes {

namespace {
const double TAU = 6.28318530717958647692528676655900577;
}

void torus(double inner, double outer, int sides, int rings) {
    if(sides < 3) sides = 3;
    if(rings < 3) rings = 3;

    // Surface of revolution: sweep a circle of radius `inner`, whose centre
    // sits `outer` from the origin, around the z axis.
    //
    //   x = (outer + inner*cos t) * cos p
    //   y = (outer + inner*cos t) * sin p
    //   z =          inner*sin t
    //
    // where t runs around the tube and p around the torus.  The normal is the
    // direction from the tube's centreline out to the surface, which is just
    // the same circle without the `outer` offset and of unit length -- so it
    // needs no cross product and no normalization.
    for(int i = 0; i < rings; i++) {
        const double p0 = TAU * static_cast<double>(i)     / rings;
        const double p1 = TAU * static_cast<double>(i + 1) / rings;

        const double cos_p0 = std::cos(p0), sin_p0 = std::sin(p0);
        const double cos_p1 = std::cos(p1), sin_p1 = std::sin(p1);

        glBegin(GL_QUAD_STRIP);

        // <= so the strip closes back onto its first pair of vertices.
        for(int j = 0; j <= sides; j++) {
            const double t = TAU * static_cast<double>(j) / sides;

            const double cos_t = std::cos(t), sin_t = std::sin(t);
            const double r = outer + inner * cos_t;

            glNormal3d(cos_t * cos_p1, cos_t * sin_p1, sin_t);
            glVertex3d(r * cos_p1, r * sin_p1, inner * sin_t);

            glNormal3d(cos_t * cos_p0, cos_t * sin_p0, sin_t);
            glVertex3d(r * cos_p0, r * sin_p0, inner * sin_t);
        }

        glEnd();
    }
}

}
}
}
