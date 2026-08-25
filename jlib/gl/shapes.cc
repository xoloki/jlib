/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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
