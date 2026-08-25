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

#include <jlib/crypt/schnorr.hh>

#include <iostream>

using namespace jlib::crypt::schnorr;
using namespace jlib::crypt::curve;

int main(int argc, char** argv) {
    // libsodium requires this before any other call into it.
    if(sodium_init() < 0) {
        std::cerr << "sodium_init() failed" << std::endl;
        return -1;
    }

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
    y = x2[0] * G + x2[1] * Commitment::H();
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
