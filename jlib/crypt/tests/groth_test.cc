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

#include <jlib/crypt/groth.hh>

using namespace jlib::crypt::groth;

void test() {
    /*
    unsigned char x[crypto_core_ristretto255_HASHBYTES];
    randombytes_buf(x, sizeof x);
    
    // Compute px = p(x), a group element derived from x
    unsigned char px[crypto_core_ristretto255_BYTES];
    crypto_core_ristretto255_from_hash(px, x);
    
    // Compute a = p(x) * g^r
    unsigned char r[crypto_core_ristretto255_SCALARBYTES];
    unsigned char gr[crypto_core_ristretto255_BYTES];
    unsigned char a[crypto_core_ristretto255_BYTES];
    crypto_core_ristretto255_scalar_random(r);
    crypto_scalarmult_ristretto255_base(gr, r);
    crypto_core_ristretto255_add(a, px, gr);
    */
    Scalar a = Scalar::random();
    Scalar b = Scalar::random();
    Scalar x = a + b;

    std::cout << a << " + " << b << " = " << x << std::endl;
}
