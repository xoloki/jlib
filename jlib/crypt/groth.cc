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

#include <ostream>

#include <sodium.h>
#include <sodium/crypto_core_ristretto255.h>

#include <jlib/crypt/groth.hh>
#include <jlib/util/util.hh>

namespace jlib {
namespace crypt {
namespace groth {

BinaryProof prove(const curve::Scalar& m, const curve::Scalar& r) {
    curve::Commitment c(m, r);
    
    curve::Scalar a = curve::Scalar::random();
    curve::Scalar s = curve::Scalar::random();
    curve::Scalar t = curve::Scalar::random();
    
    curve::Commitment c_a(a, s);
    curve::Commitment c_b(a*m, t);
    
    curve::Scalar x = curve::hash<curve::Scalar::HASHSIZE>(c, c_a, c_b);
    
    curve::Scalar f = m*x + a;
    curve::Scalar z_a = r*x + s;
    curve::Scalar z_b = r*(x - f) + t;

    return BinaryProof{c, c_a, c_b, f, z_a, z_b};
}

bool verify(const BinaryProof& proof) {
    curve::Scalar x = curve::hash<curve::Scalar::HASHSIZE>(proof.c, proof.c_a, proof.c_b);

    curve::Point cxca = x * proof.c + proof.c_a;
    curve::Point cfza = curve::Commitment(proof.f, proof.z_a);
    
    curve::Point cxfcb = (x-proof.f)*proof.c + proof.c_b;
    curve::Point czzb = curve::Commitment(curve::Scalar::zero(), proof.z_b);
    
    return (cxca == cfza && cxfcb == czzb);
}

ZeroProof prove(const std::vector<curve::Commitment>& c, std::size_t l, const curve::Scalar& r) {
    return ZeroProof();
}
    
bool verify(const ZeroProof& proof) {
    curve::Hash<curve::Scalar::HASHSIZE> xhash;
    for(curve::Commitment c : proof.c)
        xhash.update(c);
                     
    curve::Scalar x = xhash;

    return false;
}
    
}
}
}
