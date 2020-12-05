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

#include <jlib/crypt/schnorr.hh>

using namespace jlib::crypt::schnorr;
using namespace jlib::crypt::curve;

int main(int argc, char** argv) {
    BasePoint G;
    Scalar x = Scalar::random();
    Point y = x * G;
    Proof p = prove(G, y, x);
    
    if(!verify(p)) {
        std::cerr << "schnorr proof didn't verify" << std::endl;
        return -1;
    } 

    DoubleProof dp;
    Scalar s = Scalar::random();
    Scalar t = Scalar::random();
    y = dp.g * s + dp.h * t;
    dp = prove(y, s, t);

    if(!verify(dp)) {
        std::cerr << "schnorr DoubleProof didn't verify" << std::endl;
        return -1;
    } 
    
    
    Scalar x2[2];
    x2[0] = Scalar::random();
    x2[1] = Scalar::random();
    y = x2[0] * G + x2[1] * Commitment::H;
    GeneralProof<2> proof2 = prove<2>(y, x2);

    if(!verify(proof2)) {
        std::cerr << "schnorr proof<2> didn't verify" << std::endl;
        return -1;
    } 

    GeneralProof<3> proof3;
    Scalar x3[3];
    x3[0] = Scalar::random();
    x3[1] = Scalar::random();
    x3[2] = Scalar::random();
    y = x3[0] * proof3.g + x3[1] * proof3.h[0] + x3[2] * proof3.h[1];
    proof3 = prove<3>(y, x3)
;
    if(!verify(proof3)) {
        std::cerr << "schnorr proof<3> didn't verify" << std::endl;
        return -1;
    } 
    
    return 0;
}
