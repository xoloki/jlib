/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joey Yandle <dragon@dancingdragon.net>
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

#include <bitset>
#include <cmath>
#include <ostream>

#include <jlib/crypt/schnorr.hh>

namespace jlib {
namespace crypt {
namespace schnorr {

Proof prove(const curve::Point& g, const curve::Point& y, const curve::Scalar& x) {
    curve::Scalar v = curve::Scalar::random();
    curve::Point t = v * g;
    curve::Scalar c = curve::hash<curve::Scalar::HASHSIZE>(g, y, t);
    curve::Scalar r = v - c*x;
    
    return Proof{g, y, r, t};
}

bool verify(const Proof& proof) {
    curve::Scalar c = curve::hash<curve::Scalar::HASHSIZE>(proof.g, proof.y, proof.t);
    curve::Point p = proof.r * proof.g + c * proof.y;
    
    return (p == proof.t);
}

}
}
}
