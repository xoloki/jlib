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

#ifndef JLIB_GL_SHAPES_HH
#define JLIB_GL_SHAPES_HH

namespace jlib {
namespace gl {
namespace shapes {

/**
 * A solid torus about the z axis, with per-vertex normals.
 *
 * Replaces glutSolidTorus, which was the last GLUT geometry helper jlib used
 * and so the last thing tying jgltorus to a library that is deprecated on
 * macOS and X11-only elsewhere.  Same argument order and the same meaning:
 *
 *   inner   radius of the tube itself
 *   outer   distance from the origin to the centre of the tube
 *   sides   subdivisions around the tube's cross-section
 *   rings   subdivisions around the torus
 *
 * Normals are emitted per vertex, so this lights correctly rather than
 * appearing flat -- which also makes it the first piece of real lit geometry
 * in the library, ahead of the solid rendering work.
 */
void torus(double inner, double outer, int sides, int rings);

}
}
}

#endif //JLIB_GL_SHAPES_HH
