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

#ifndef JLIB_CRYPT_GROTH_HH
#define JLIB_CRYPT_GROTH_HH

#include <ostream>

#include <sodium.h>
#include <sodium/crypto_core_ristretto255.h>

#include <jlib/util/util.hh>
#include <jlib/crypt/curve.hh>

namespace jlib {
namespace crypt {
namespace groth {

// a BinaryProof is a proof that the commitment c opens to either 0 or 1
struct BinaryProof {
    curve::Commitment c;
    curve::Commitment c_a;
    curve::Commitment c_b;
    curve::Scalar f;
    curve::Scalar z_a;
    curve::Scalar z_b;
};
    
BinaryProof prove(const curve::Scalar& m, const curve::Scalar& r);

bool verify(const BinaryProof& proof);

// a ZeroProof is a proof that one of the many commitments opens to zero
struct ZeroProof {
    std::vector<curve::Commitment> c;
    std::vector<curve::Commitment> c_l;
    std::vector<curve::Commitment> c_a;
    std::vector<curve::Commitment> c_b;
    std::vector<curve::Point> c_d;
    std::vector<curve::Scalar> f;
    std::vector<curve::Scalar> z_a;
    std::vector<curve::Scalar> z_b;
};
    
ZeroProof prove(const std::vector<curve::Commitment>& c, std::size_t l, const curve::Scalar& r);

bool verify(const ZeroProof& proof);

}
}
}

#endif
