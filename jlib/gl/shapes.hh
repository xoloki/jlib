/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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
