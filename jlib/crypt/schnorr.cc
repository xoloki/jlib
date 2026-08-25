/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2020 Joey Yandle <xoloki@gmail.com>
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

DoubleProof prove(const curve::Point& y, const curve::Scalar& s, const curve::Scalar& t) {
    DoubleProof proof;

    proof.y = y;
    
    curve::Scalar s0 = curve::Scalar::random();
    curve::Scalar t0 = curve::Scalar::random();

    proof.u = (proof.g * s0) + (proof.h * t0);

    curve::Scalar c = curve::hash<curve::Scalar::HASHSIZE>(proof.y, proof.u);

    proof.s = (s0 - (c * s));
    proof.t = (t0 - (c * t));
    
    return proof;
}

bool verify(const DoubleProof& proof) {
    curve::Scalar c = curve::hash<curve::Scalar::HASHSIZE>(proof.y, proof.u);

    curve::Point u = (proof.y * c) + (proof.g * proof.s) + (proof.h * proof.t);

    return (u == proof.u);
}

}
}
}
