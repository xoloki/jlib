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

#ifndef JLIB_CRYPT_SCHNORR_HH
#define JLIB_CRYPT_SCHNORR_HH

#include <ostream>

#include <sodium.h>
#include <sodium/crypto_core_ristretto255.h>

#include <jlib/util/util.hh>
#include <jlib/crypt/curve.hh>

namespace jlib {
namespace crypt {
namespace schnorr {

// a schnorr::Proof is a proof that the maker knew the secret key x
// such that x * g == y
struct Proof {
    curve::Point g;
    curve::Point y;
    curve::Scalar r;
    curve::Point t;
};

Proof prove(const curve::Point& g, const curve::Point& y, const curve::Scalar& x);

bool verify(const Proof& proof);

}
}
}

#endif
