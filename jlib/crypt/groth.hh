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

#ifndef JLIB_CRYPT_GROTH_HH
#define JLIB_CRYPT_GROTH_HH

#include <cstddef>
#include <vector>

#include <jlib/crypt/curve.hh>

namespace jlib {
namespace crypt {
namespace groth {

/*
 * Groth-Kohlweiss one-out-of-many proofs over ristretto255.
 *
 * Jens Groth and Markulf Kohlweiss, "One-out-of-Many Proofs: Or How to Leak a
 * Secret and Spend a Coin", EUROCRYPT 2015.  The two proofs here are Fig. 1
 * (section 2.3) and Fig. 2 (section 3), and the code is meant to read as those
 * figures do -- every equation is cited at the site that implements it, so a
 * reader can check this against the paper rather than against its author.
 *
 * Two differences in notation, both of them everywhere:
 *
 *   - The paper writes the group multiplicatively, so a commitment raised to a
 *     scalar is c^x and a product of commitments is a product.  curve::Point
 *     is additive: c^x is `x * C`, and the products become sums.
 *   - The paper indexes j from 1 to n and k from 0 to n-1, with k = j-1.  Here
 *     both run from 0 to n-1, so the code's j is the paper's k.
 *
 * Figs. 1 and 2 are interactive; these are their Fiat-Shamir versions, and the
 * paper gives that transform too -- Fig. 3, where it builds a ring signature
 * on Fig. 2 with
 *
 *   x = H(ck, M, R, a)
 *
 * for commitment key ck, signed message M, statement R and the Sigma-
 * protocol's initial message a.  challenge() follows it term for term, with
 * one deliberate omission: M.
 *
 * Omitting M is what makes these proofs of knowledge rather than signatures.
 * Fig. 3's whole purpose is to sign a message, and a ZeroProof with no message
 * attests only that the prover knew an opening -- it says nothing about anyone
 * assenting to anything, and it is replayable by whoever holds a copy.  A
 * one-out-of-many proof is not usually used to sign, so there is no message
 * parameter here; schnorr::prove takes one, and the distinction is the same
 * there.
 *
 * Naming follows the paper, with the house convention that curve points are
 * uppercase and scalars are lowercase.  So C, C_a, C_b, C_d are commitments
 * (points) and f, z_a, z_b, z_d are the scalar responses to the challenge.
 *
 * Both proofs are non-interactive by Fiat-Shamir: the challenge x is a hash
 * of every first-round commitment.  Every one of them has to be in that
 * hash.  A commitment left out is one the prover can still choose after
 * seeing x, and both verifying equations are linear in C_l, so a prover who
 * can pick it afterwards can solve for one that passes the bit check while
 * committing to something that is not a bit.  challenge() is therefore
 * written once and shared by prove() and verify(), so they cannot drift.
 */

/*
 * A BinaryProof shows that C opens to 0 or 1 without revealing which.
 * Fig. 1, section 2.3 -- "Sigma-protocol for commitment to m in {0,1}".
 *
 * Note which of these is the statement and which the prover's first message:
 * the paper's statement is (ck, c) and its initial message is (c_a, c_b)
 * alone.  C is here because a Fiat-Shamir challenge has to bind the statement
 * as well as the first message.
 */
struct BinaryProof {
    curve::Commitment C;    // the commitment being proven binary
    curve::Commitment C_a;
    curve::Commitment C_b;
    curve::Scalar f;
    curve::Scalar z_a;
    curve::Scalar z_b;
};

// Proves that the commitment to m with blinding factor r opens to a bit.
// Throws if m is neither zero nor one -- there is no proof to be had, and
// returning one that cannot verify is a worse answer than saying so.
BinaryProof prove(const curve::Scalar& m, const curve::Scalar& r);

bool verify(const BinaryProof& proof);

// A ZeroProof shows that one of the commitments in C opens to zero, without
// revealing which one.  Fig. 2, section 3.
//
// The secret index is proven bit by bit -- the paper's own construction, n
// parallel instances of the section 2.3 protocol above -- so every vector but
// C has exactly one entry per index bit n, and C holds exactly 2^n
// commitments.
//
// Those lengths are part of the proof, which means they come from whoever is
// trying to convince us and are exactly as untrusted as its contents.
// verify() establishes the shape before it indexes anything.
struct ZeroProof {
    std::vector<curve::Commitment> C;      // 2^n commitments, one of which is zero
    std::vector<curve::Commitment> C_l;    // n: commitments to the bits of the secret index
    std::vector<curve::Commitment> C_a;    // n
    std::vector<curve::Commitment> C_b;    // n
    std::vector<curve::Point>      C_d;    // n
    std::vector<curve::Scalar>     f;      // n
    std::vector<curve::Scalar>     z_a;    // n
    std::vector<curve::Scalar>     z_b;    // n
    curve::Scalar z_d;
};

// Proves that C[l] opens to zero with blinding factor r.
//
// C is padded up to the next power of two by repeating its last element, so
// the proof's own C may be longer than the one passed in.  Throws if C holds
// fewer than two commitments (one hides nothing), if l is out of range, or
// if C is too long to index.
ZeroProof prove(const std::vector<curve::Commitment>& C, std::size_t l, const curve::Scalar& r);

// Returns false for a malformed proof as readily as for a false one: a
// verifier has nothing to say about the difference, and throwing would hand
// the prover a way to pick which failure the caller sees.
bool verify(const ZeroProof& proof);

}
}
}

#endif
