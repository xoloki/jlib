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

#include <ostream>

#include <jlib/crypt/schnorr.hh>

namespace jlib {
namespace crypt {
namespace schnorr {

Proof prove(const curve::Point& G, const curve::Point& Y, const curve::Scalar& x,
            const std::optional<std::string>& message)
{
    Proof proof;

    const curve::Scalar k = curve::Scalar::random();

    proof.R = k * G;

    // One generator, so the single-generator case of the same challenge the
    // template uses -- shared rather than written twice, because a prove and a
    // verify that hash different things is the one bug this scheme cannot
    // survive and the easiest one to introduce by duplication.
    const Generators<1> gens{ G };
    const curve::Scalar c = challenge<1>(gens, Y, proof.R, message);

    proof.s = k - (c * x);

    return proof;
}

bool verify(const Proof& proof, const curve::Point& G, const curve::Point& Y,
            const std::optional<std::string>& message)
{
    const Generators<1> gens{ G };
    const curve::Scalar c = challenge<1>(gens, Y, proof.R, message);

    const curve::Point P = (proof.s * G) + (c * Y);

    return (P == proof.R);
}

}
}
}
