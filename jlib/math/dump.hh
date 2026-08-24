/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_MATH_DUMP_HH
#define JLIB_MATH_DUMP_HH

#include <jlib/math/matrix.hh>

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <string>

namespace jlib {
namespace math {

/**
 * Write one frame of projected coordinates to a file, for comparing one
 * projection pipeline against another.
 *
 * Set JLIB_PLOT_DUMP to a path and the first frame's source and transformed
 * vertices are written there, then the file is closed and every later call is
 * a no-op.  One frame only, because the objects rotate: two runs are only
 * comparable at the same rotation, and frame zero is the one state both
 * pipelines are guaranteed to share.
 *
 * This exists because "does it look right" is a poor oracle for a projection
 * chain.  The perspective divide being a no-op after the first step, and half
 * the vertices being mirrored through the origin by a frustum that straddles
 * the eye, both look like "wrong somehow" -- and fixing only one of them looks
 * like no improvement at all, which is how a correct fix comes to be doubted.
 */
namespace dump {

inline std::ofstream* stream() {
    static bool tried = false;
    static std::ofstream* out = 0;

    if(!tried) {
        tried = true;
        const char* path = std::getenv("JLIB_PLOT_DUMP");
        if(path != 0 && path[0] != '\0') {
            out = new std::ofstream(path);
            if(!out->good()) {
                delete out;
                out = 0;
            }
        }
    }

    return out;
}

inline bool active() {
    return stream() != 0;
}

/**
 * Called once the first frame is complete; closes the file so later frames,
 * at other rotations, do not append to it.
 */
inline void frame_done() {
    std::ofstream* out = stream();
    if(out != 0) {
        out->flush();
        out->close();
    }
}

/**
 * One source vertex and what the projection chain made of it.
 */
template<typename T>
inline void vertex(const std::string& tag,
                   const math::vertex<T>& src,
                   const math::vertex<T>& dst)
{
    std::ofstream* out = stream();
    if(out == 0 || !out->is_open())
        return;

    *out << tag << "  src";
    for(unsigned int i = 0; i < src.D; i++)
        *out << " " << std::fixed << std::setprecision(6) << src[i];

    *out << "  ->  dst";
    for(unsigned int i = 0; i < dst.D; i++)
        *out << " " << std::fixed << std::setprecision(6) << dst[i];

    // The homogeneous coordinate: it should be 1 after a correct divide, and
    // its sign is what flips when the frustum straddles the eye.
    *out << "  w " << std::fixed << std::setprecision(6) << dst[dst.D];

    *out << "\n";
}

}

}
}

#endif //JLIB_MATH_DUMP_HH
