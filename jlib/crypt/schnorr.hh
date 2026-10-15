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

#include <array>
#include <cstddef>
#include <optional>
#include <ostream>
#include <string>

#include <sodium.h>
#include <sodium/crypto_core_ristretto255.h>

#include <jlib/util/util.hh>
#include <jlib/crypt/curve.hh>

namespace jlib {
namespace crypt {
namespace schnorr {

/**
 * Non-interactive proofs of knowledge of a discrete logarithm, Fiat-Shamir
 * over ristretto255.
 *
 * **The proof is `(R, s)` and nothing else.**  The statement being proved --
 * the generators and the public point -- is an argument to both `prove` and
 * `verify`, never a member of the proof.
 *
 * That is not a style choice.  These structures used to carry their own `g`
 * and `y`, and `verify(proof)` read both out of the proof, so a `true` return
 * meant "this is a valid proof about whatever point it contains" rather than
 * "a valid proof about the key you care about".  A caller had to compare the
 * proof's own fields against the expected ones, separately, and nothing said
 * so.  Now the statement cannot be omitted because there is nowhere to put it.
 *
 * Naming follows the usual convention: **upper case for curve points, lower
 * case for scalars.**
 *
 * `message` binds the proof to a byte string, Fiat-Shamir style, by including
 * it in the challenge.  Without one these are proofs of knowledge and not
 * signatures: a proof with no message is equally valid attached to anything.
 */

/** A proof of knowledge of `x` such that `Y == x * G`. */
struct Proof {
    curve::Point  R;   ///< the commitment, `k * G`
    curve::Scalar s;   ///< the response, `k - c*x`
};

/**
 * Prove knowledge of `x` with `Y == x * G`.
 *
 * The challenge is `c = H(G, Y, R [, message])`.  G is in the hash because a
 * challenge that omits a generator lets the generator be substituted after the
 * fact without changing anything that was committed to.
 */
Proof prove(const curve::Point& G, const curve::Point& Y, const curve::Scalar& x,
            const std::optional<std::string>& message = std::nullopt);

/** Check `s*G + c*Y == R`.  The message must match the one proved, if any. */
bool verify(const Proof& proof, const curve::Point& G, const curve::Point& Y,
            const std::optional<std::string>& message = std::nullopt);

/** N generators, the statement for a MultiProof. */
template<std::size_t N>
using Generators = std::array<curve::Point, N>;

/**
 * A proof of knowledge of `x[0..N-1]` such that `Y == sum(x[i] * G[i])`.
 *
 * This replaces both `DoubleProof` and `GeneralProof<N>`, which were the same
 * scheme written twice -- `DoubleProof` was exactly the N=2 case.  Both also
 * hashed a challenge that omitted every generator, and both kept the
 * generators inside the proof where a deserialised one could carry anything.
 */
template<std::size_t N>
struct MultiProof {
    curve::Point  R;      ///< the commitment, `sum(k[i] * G[i])`
    curve::Scalar s[N];   ///< the responses, `k[i] - c*x[i]`
};

/**
 * The default generator chain: `G[0]` is the ristretto255 base point and each
 * subsequent generator is the hash of the one before it, so no discrete-log
 * relation between them is known.
 *
 * Built once on first use.  It used to be recomputed in every proof's
 * constructor, which meant N-1 hashes per object.
 */
template<std::size_t N>
const Generators<N>& generators();

template<std::size_t N>
MultiProof<N> prove(const Generators<N>& G, const curve::Point& Y,
                    const curve::Scalar (&x)[N],
                    const std::optional<std::string>& message = std::nullopt);

template<std::size_t N>
bool verify(const MultiProof<N>& proof, const Generators<N>& G, const curve::Point& Y,
            const std::optional<std::string>& message = std::nullopt);

/* ------------------------------------------------------------------ detail */

/** The challenge, shared by prove and verify so they cannot disagree. */
template<std::size_t N>
curve::Scalar challenge(const Generators<N>& G, const curve::Point& Y,
                        const curve::Point& R,
                        const std::optional<std::string>& message)
{
    curve::Hash<curve::Scalar::HASHSIZE> h;

    for(std::size_t i = 0; i < N; i++)
        h.update(G[i]);

    h.update(Y);
    h.update(R);

    // A presence byte, so that no message and an empty message are different
    // challenges.  Updating a hash with zero bytes is the same as not
    // updating it, so without this a bare proof of knowledge -- which is not
    // a signature on anything -- verified as a signature on "", and a
    // signature on "" verified as a bare proof of knowledge.
    //
    // The length goes in with it.  The message is last and everything before
    // it is fixed-size, so nothing is ambiguous today, but a hash whose final
    // field is unlengthed is one field away from being so, and this costs
    // eight bytes.
    if(message) {
        const unsigned char present = 0x01;
        h.update(&present, 1);
        h.update(curve::Scalar(message->size()));
        h.update(reinterpret_cast<const unsigned char*>(message->data()), message->size());
    } else {
        const unsigned char absent = 0x00;
        h.update(&absent, 1);
    }

    h.finalize();

    return curve::Scalar(h);
}

template<std::size_t N>
const Generators<N>& generators() {
    static const Generators<N> G = [] {
        Generators<N> g;

        g[0] = curve::BasePoint();
        for(std::size_t i = 1; i < N; i++)
            g[i] = curve::Point(curve::hash<curve::Point::HASHSIZE>(g[i-1]));

        return g;
    }();

    return G;
}

template<std::size_t N>
MultiProof<N> prove(const Generators<N>& G, const curve::Point& Y,
                    const curve::Scalar (&x)[N],
                    const std::optional<std::string>& message)
{
    MultiProof<N> proof;

    curve::Scalar k[N];

    for(std::size_t i = 0; i < N; i++)
        k[i] = curve::Scalar::random();

    proof.R = k[0] * G[0];
    for(std::size_t i = 1; i < N; i++)
        proof.R += (k[i] * G[i]);

    const curve::Scalar c = challenge<N>(G, Y, proof.R, message);

    for(std::size_t i = 0; i < N; i++)
        proof.s[i] = k[i] - (c * x[i]);

    return proof;
}

template<std::size_t N>
bool verify(const MultiProof<N>& proof, const Generators<N>& G, const curve::Point& Y,
            const std::optional<std::string>& message)
{
    const curve::Scalar c = challenge<N>(G, Y, proof.R, message);

    curve::Point P = c * Y;

    for(std::size_t i = 0; i < N; i++)
        P += (proof.s[i] * G[i]);

    return (P == proof.R);
}

}
}
}

#endif
