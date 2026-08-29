/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_MATH_DUMP_HH
#define JLIB_MATH_DUMP_HH

#include <jlib/math/matrix.hh>

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>

namespace jlib {
namespace math {

/**
 * Write one frame of projected coordinates to a file, for comparing one
 * projection pipeline against another.
 *
 * Set JLIB_PLOT_DUMP to a path and the first frame's source and transformed
 * vertices are written there, then the file is closed and every later call is
 * a no-op.  One frame by default, because the objects rotate: two runs are
 * only comparable at the same rotation, and frame zero is the one state both
 * pipelines are guaranteed to share.
 *
 * Two more variables widen that when frame zero is not the interesting one:
 *
 *   JLIB_PLOT_DUMP_SKIP    frames to pass over before capturing (default 0)
 *   JLIB_PLOT_DUMP_FRAMES  frames to capture once started (default 1)
 *
 * Each captured frame is preceded by a "frame N" line, N counting from zero
 * over the whole run, so a reader can split the file without guessing.
 *
 * The rotation caveat does not go away by skipping: comparing frame 300 of two
 * runs only means something if both are deterministic up to frame 300.  What
 * skipping is for is state that only exists after something has happened --
 * a keypress that changes the dimension, say, where frame zero is by
 * definition the state before the thing being investigated (#124).
 *
 * This exists because "does it look right" is a poor oracle for a projection
 * chain.  The perspective divide being a no-op after the first step, and half
 * the vertices being mirrored through the origin by a frustum that straddles
 * the eye, both look like "wrong somehow" -- and fixing only one of them looks
 * like no improvement at all, which is how a correct fix comes to be doubted.
 */
namespace dump {

/** Everything the dump remembers between calls. */
struct state {
    // Owned, where this was a leaked `new std::ofstream`.  The leak was not
    // the problem; the lost output was.  Only frame_done() flushed, and a
    // process that exits without reaching one -- a crash, an exit(0) from a
    // key handler, or any caller that transforms without drawing -- left the
    // buffer unwritten and the file empty.  A static's destructor runs on a
    // normal exit, so the dump survives one now.
    std::unique_ptr<std::ofstream> out;

    unsigned long skip = 0;      // frames to pass over before capturing
    unsigned long want = 1;      // frames to capture once started
    unsigned long seen = 0;      // frames begun, captured or not
    unsigned long kept = 0;      // frames written in full

    bool open = false;           // a frame's marker has been written
    bool tried = false;          // the environment has been read
};

inline unsigned long from_env(const char* name, unsigned long fallback) {
    const char* v = std::getenv(name);

    if(v == 0 || v[0] == '\0')
        return fallback;

    const long n = std::strtol(v, 0, 10);

    return (n < 0) ? fallback : static_cast<unsigned long>(n);
}

inline state& current() {
    static state s;

    if(!s.tried) {
        s.tried = true;

        const char* path = std::getenv("JLIB_PLOT_DUMP");

        if(path != 0 && path[0] != '\0') {
            s.out.reset(new std::ofstream(path));

            if(!s.out->good())
                s.out.reset();
        }

        s.skip = from_env("JLIB_PLOT_DUMP_SKIP", 0);
        s.want = from_env("JLIB_PLOT_DUMP_FRAMES", 1);
    }

    return s;
}

/** True while this frame is one of the ones being captured. */
inline bool capturing() {
    state& s = current();

    return s.out && s.seen >= s.skip && s.kept < s.want;
}

/**
 * Forget everything and re-read the environment.
 *
 * For tests, which need more than one configuration per process and would
 * otherwise be stuck with whichever one ran first.
 */
inline void reset() {
    state& s = current();

    if(s.out) {
        s.out->flush();
        s.out->close();
    }

    s = state();
}

inline std::ofstream* stream() {
    return capturing() ? current().out.get() : 0;
}

inline bool active() {
    return stream() != 0;
}

/**
 * Called once the first frame is complete; closes the file so later frames,
 * at other rotations, do not append to it.
 */
inline void frame_done() {
    state& s = current();

    if(!s.out)
        return;

    // A frame counts as captured only if something was actually written to
    // it.  A plot that drew nothing this frame should not consume one of the
    // frames asked for.
    if(s.open) {
        s.open = false;
        s.kept++;

        s.out->flush();

        if(s.kept >= s.want) {
            s.out->close();
            s.out.reset();
        }
    }

    s.seen++;
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

    state& s = current();

    // Lazily, so the marker sits immediately above the frame's first vertex
    // and a frame that drew nothing leaves no trace at all.
    if(!s.open) {
        s.open = true;
        *out << "frame " << s.seen << "\n";
    }

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
