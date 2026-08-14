/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 1999 Joe Yandle <joey@divisionbyzero.com>
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
 */

#ifndef JLIB_APPS_COLOR_HH
#define JLIB_APPS_COLOR_HH

#include <cmath>
#include <vector>

namespace jlib {
namespace apps {

const long double PI = 3.14159265358979323846264338;

template<class O>
struct triple {
    O r;
    O g;
    O b;
};

/**
 * A hue, at full saturation and value.
 *
 * Colours are kept as hues rather than triples so they can be combined
 * without washing out.  Averaging in RGB drives every channel toward its
 * mean: four random corners average to a standard deviation of about 0.14
 * around 0.5, so everything built from several vertices came out the same
 * olive grey no matter which vertices it joined.  A hue has nowhere to
 * collapse to -- combining hues always gives another fully saturated colour.
 */
template<typename O>
inline
triple<O> hsv(O h) {
    h -= std::floor(h);

    const O sector = h * 6;
    const int i = static_cast<int>(std::floor(sector));
    const O f = sector - i;

    const O q = 1 - f;
    const O t = f;

    triple<O> c;
    switch(i % 6) {
    case 0: c.r = 1; c.g = t; c.b = 0; break;
    case 1: c.r = q; c.g = 1; c.b = 0; break;
    case 2: c.r = 0; c.g = 1; c.b = t; break;
    case 3: c.r = 0; c.g = q; c.b = 1; break;
    case 4: c.r = t; c.g = 0; c.b = 1; break;
    default: c.r = 1; c.g = 0; c.b = q; break;
    }

    return c;
}

/**
 * Where a set of hues sits on the colour wheel.
 *
 * The mean direction, not the arithmetic mean: hues wrap, so averaging 0.9
 * and 0.1 numerically gives 0.5 -- cyan from two reds.  Summing unit vectors
 * and taking the angle gets the answer that agrees with the wheel.
 *
 * Hues spread evenly around the wheel sum to nothing and have no mean
 * direction at all.  That is a real ambiguity rather than a numerical one, so
 * it falls back to the first instead of pretending otherwise.
 */
template<typename O>
inline
O hue_mean(const std::vector<O>& h) {
    if(h.empty()) return 0;

    O x = 0, y = 0;
    for(unsigned int k = 0; k < h.size(); k++) {
        x += std::cos(2 * PI * h[k]);
        y += std::sin(2 * PI * h[k]);
    }

    if(std::sqrt(x * x + y * y) < 1e-9)
        return h[0];

    const O a = std::atan2(y, x) / (2 * PI);

    return a - std::floor(a);
}

/**
 * The hue for vertex i, spaced by the golden ratio.
 *
 * Successive vertices land as far apart on the wheel as they can, so
 * neighbours stay distinguishable at any dimensionality, and the figure comes
 * up the same colours every run -- which matters when comparing two builds by
 * eye.  Random hues clump: at 32 vertices some pair is almost always within a
 * couple of degrees.
 */
template<typename O>
inline
O golden_hue(unsigned int i) {
    const O golden = 0.6180339887498948;

    return std::fmod(i * golden, 1.0);
}

}
}

#endif
