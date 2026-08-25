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

#ifndef JLIB_CRYPT_SCHNORR_HH
#define JLIB_CRYPT_SCHNORR_HH

#include <ostream>
#include <iterator>

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

struct DoubleProof {
    curve::Point g;
    curve::Point h;
    curve::Point y;
    curve::Scalar s;
    curve::Scalar t;
    curve::Point u;

    DoubleProof() {
        g = curve::BasePoint();
        h = curve::hash<curve::Point::HASHSIZE>(g);
    }
};

DoubleProof prove(const curve::Point& y, const curve::Scalar& s, const curve::Scalar& t);

bool verify(const DoubleProof& proof);

template<int N>
struct GeneralProof {
    curve::Point g;
    curve::Point h[N-1];
    curve::Point y;
    curve::Scalar r[N];
    curve::Point t;

    GeneralProof() {
        g = curve::BasePoint();
        for(int i = 0; i < N-1; i++) {
            if(i == 0) {
                h[0] = curve::hash<curve::Point::HASHSIZE>(g);
            } else {
                h[i] = curve::hash<curve::Point::HASHSIZE>(h[i-1]);
            }
        }
    }
};

template<int N>
GeneralProof<N> prove(const curve::Point& y, const curve::Scalar (&x)[N]);

template<int N>
bool verify(const GeneralProof<N>& proof);

template<int N>
GeneralProof<N> prove(const curve::Point& y, const curve::Scalar (&x)[N]) {
    if(sizeof(x) != (N * sizeof(*x)))
        throw std::runtime_error("x is not the same size as N");

    GeneralProof<N> proof;

    proof.y = y;

    curve::Scalar r[N];
    
    for(int i = 0; i < N; i++) {
        r[i] = curve::Scalar::random();
    }
    
    proof.t = r[0] * proof.g;
    for(int i = 0; i < (N-1); i++) {
        proof.t += (r[i+1] * proof.h[i]);
    }

    // hash y,t for the challenge
    curve::Scalar c = curve::hash<curve::Scalar::HASHSIZE>(proof.y, proof.t);
        
    //curve::Scalar r = v - c*x;
    for(int i = 0; i < N; i++) {
        proof.r[i] = (r[i] - (c * x[i]));
    }
    
    return proof;
}

template<int N>
bool verify(const GeneralProof<N>& proof) {
    curve::Scalar c = curve::hash<curve::Scalar::HASHSIZE>(proof.y, proof.t);

    curve::Point p = proof.y * c;
    p += (proof.r[0] * proof.g);
    for(int i = 0; i < N-1; i++) {
        p += (proof.r[i+1] * proof.h[i]);
    }
    
    return (p == proof.t);
}
    
}
}
}

#endif
