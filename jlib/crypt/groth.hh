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
    curve::Scalar z_d;
};
    
ZeroProof prove(const std::vector<curve::Commitment>& c, std::size_t l, const curve::Scalar& r);

bool verify(const ZeroProof& proof);

}
}
}

#endif
