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
#include <limits>
#include <stdexcept>

#include <jlib/crypt/groth.hh>
#include <jlib/math/polynomial.hh>

namespace jlib {
namespace crypt {
namespace groth {

namespace {

typedef math::Polynomial<curve::Scalar, curve::Scalar::Power> Polynomial;

// The secret index is split into bits by a std::bitset, and 2^n commitments
// have to be addressable by a std::size_t, so the bitset is sized to match
// and n is bounded by its width.  That bound is also what makes the shift in
// commitment_count() defined.
const std::size_t INDEX_BITS = std::numeric_limits<std::size_t>::digits;

typedef std::bitset<INDEX_BITS> Index;

// The Fiat-Shamir challenge for a BinaryProof: the commitment key, then the
// statement C, then the first message (C_a, C_b).  Fig. 1's statement is
// (ck, c) and its initial message is c_a, c_b alone, and ck is part of the
// statement, so it is hashed too -- see the ZeroProof challenge below for why
// that is not optional.
//
// Takes the commitments rather than the proof so that prove() can call it
// before it has the responses that would complete one.
curve::Scalar challenge(const curve::Commitment& C, const curve::Commitment& C_a,
                        const curve::Commitment& C_b) {
    return curve::Scalar(curve::hash<curve::Scalar::HASHSIZE>(
        curve::Commitment::G(), curve::Commitment::H(), C, C_a, C_b));
}

// The Fiat-Shamir challenge for a ZeroProof.
//
// Fig. 2 is interactive -- the verifier picks x <- {0,1}^lambda after
// receiving the whole first message -- but the paper does give the transform,
// in Fig. 3, where it builds a ring signature on exactly this protocol:
//
//   x = H(ck, M, R, a)
//
// with ck the commitment key, M the message signed, R = (c_0, ..., c_{N-1})
// the statement, and a the Sigma-protocol's initial message.  Section 4 says
// it in prose as well: the challenge is a hash of "the initial message in the
// Sigma-protocol and the message to be signed".
//
// So, term by term:
//
//   ck  the commitment key, here the pair (G, H).  Both are build-time
//       constants, so no prover can substitute one today -- but neither could
//       they in Fig. 3, and the paper hashes it anyway.  It is also what
//       schnorr::challenge already does with its generators, for the reason
//       written there, and groth had no business disagreeing with it.
//   M   deliberately absent.  A ZeroProof is not usually used to sign
//       anything, so there is no message parameter; see the note in groth.hh
//       about what that means.
//   R   proof.C.
//   a   proof.C_l, C_a, C_b, C_d -- all of it.  Fig. 2 names that message
//       "c_l1, ca1, cb1, cd0, ..., c_ln, can, cbn, cd(n-1)", and the
//       soundness proof rewinds "n + 1 different challenges on the same
//       initial message c_l1, ..., cd(n-1)".  C_l is the first element of it.
//       This used to hash c, c_a, c_b, c_d and leave C_l out, so C_l was a
//       commitment the prover could still choose after seeing the challenge --
//       and both bit equations are linear in it.
//
// n is bound first.  The paper has no such term; the shape of a proof is
// fixed in Fig. 2 and arrives over a wire here, so binding it explicitly
// beats leaving it implied by the byte count.
curve::Scalar challenge(const ZeroProof& proof, std::size_t n) {
    curve::Hash<curve::Scalar::HASHSIZE> hasher;

    hasher.update(curve::Scalar(n));

    // ck
    hasher.update(curve::Commitment::G());
    hasher.update(curve::Commitment::H());

    // R
    for(const curve::Commitment& C : proof.C)
        hasher.update(C);

    // a
    for(const curve::Commitment& C : proof.C_l)
        hasher.update(C);
    for(const curve::Commitment& C : proof.C_a)
        hasher.update(C);
    for(const curve::Commitment& C : proof.C_b)
        hasher.update(C);
    for(const curve::Point& D : proof.C_d)
        hasher.update(D);

    hasher.finalize();

    return curve::Scalar(hasher);
}

// ceil(log2(size)), by integer arithmetic.  std::log2 of a large power of
// two can land a hair either side of the integer and std::ceil then gives
// two different answers, which here would be two different proof sizes.
// Not a property to leave to the rounding mode.
std::size_t index_bits_for(std::size_t size) {
    std::size_t n = 0;

    while(n < INDEX_BITS && (std::size_t(1) << n) < size)
        ++n;

    return n;
}

// verify() indexes seven vectors by bit position and one by commitment
// index, all with lengths taken from the proof itself.  This returns the
// number of index bits when every one of those lengths agrees, and zero when
// any of them does not.  Zero is never a valid proof -- one commitment hides
// nothing among the others -- so a caller can read it as "malformed".
//
// Fig. 2 asks for this.  Its first acceptance condition is not an equation at
// all: "Accept if and only if c_l1, ..., c_d(n-1) in C_ck [and] f_1, ..., z_d
// in Z_q" -- every group element really a group element and every scalar
// really a scalar, before any of the equations are evaluated.  Here that is
// this function for the counts, and Point::from_bytes and
// Scalar::from_bytes for the elements themselves.
std::size_t index_bits(const ZeroProof& proof) {
    const std::size_t n = proof.C_l.size();

    // Bound n before using it as a shift count, and reject the degenerate
    // single-commitment case.
    if(n == 0 || n >= INDEX_BITS)
        return 0;

    if(proof.C.size() != (std::size_t(1) << n))
        return 0;

    if(proof.C_a.size() != n || proof.C_b.size() != n || proof.C_d.size() != n ||
       proof.f.size()   != n || proof.z_a.size() != n || proof.z_b.size() != n)
        return 0;

    return n;
}

}

BinaryProof prove(const curve::Scalar& m, const curve::Scalar& r) {
    // The prover knows m, so m not being a bit is a mistake in the calling
    // code rather than a proof that happens to be false.  Say so, instead of
    // handing back a proof that cannot verify.
    if(m != curve::Scalar::zero() && m != curve::Scalar::one())
        throw std::invalid_argument("groth::prove: m is not zero or one, there is no binary proof");

    // Fig. 1: a, s, t <- Z_q;  c_a = Com(a, s);  c_b = Com(am, t)
    const curve::Commitment C(m, r);

    const curve::Scalar a = curve::Scalar::random();
    const curve::Scalar s = curve::Scalar::random();
    const curve::Scalar t = curve::Scalar::random();

    const curve::Commitment C_a(a, s);
    const curve::Commitment C_b(a*m, t);

    const curve::Scalar x = challenge(C, C_a, C_b);

    // Fig. 1: f = mx + a;  z_a = rx + s;  z_b = r(x - f) + t
    const curve::Scalar f = m*x + a;
    const curve::Scalar z_a = r*x + s;
    const curve::Scalar z_b = r*(x - f) + t;

    return BinaryProof{C, C_a, C_b, f, z_a, z_b};
}

bool verify(const BinaryProof& proof) {
    const curve::Scalar x = challenge(proof.C, proof.C_a, proof.C_b);

    // Fig. 1:  c^x c_a = Com(f, z_a)
    const curve::Point A_lhs = x * proof.C + proof.C_a;
    const curve::Point A_rhs = curve::Commitment(proof.f, proof.z_a);

    // Fig. 1:  c^(x-f) c_b = Com(0, z_b)
    const curve::Point B_lhs = (x - proof.f) * proof.C + proof.C_b;
    const curve::Point B_rhs = curve::Commitment(curve::Scalar::zero(), proof.z_b);

    return A_lhs == A_rhs && B_lhs == B_rhs;
}

ZeroProof prove(const std::vector<curve::Commitment>& C, std::size_t l, const curve::Scalar& r) {
    if(C.size() < 2)
        throw std::invalid_argument("groth::prove: need at least two commitments to hide among");
    if(l >= C.size())
        throw std::invalid_argument("groth::prove: index is outside the commitment list");

    const std::size_t n = index_bits_for(C.size());

    if(n == 0 || n >= INDEX_BITS)
        throw std::invalid_argument("groth::prove: too many commitments to index");

    ZeroProof proof;

    // Pad up to 2^n by repeating the last commitment, so that every index of
    // n bits names one.  This is the paper's own step, section 3: "We can
    // always copy some commitments in the statement, so let us without loss of
    // generality assume N = 2^n."  A repeat is sound because the padding is
    // public and the real index is never among it.
    proof.C = C;
    proof.C.resize(std::size_t(1) << n, C.back());

    const std::size_t N = proof.C.size();

    const Index l_j(l);

    std::vector<curve::Scalar> r_j, a, s, t, rho;

    // In the paper j runs from 1 to n and k from 0 to n-1; this is code, so
    // both run from 0 to n-1.
    for(std::size_t j = 0; j < n; j++) {
        r_j.push_back(curve::Scalar::random());
        a.push_back(curve::Scalar::random());
        s.push_back(curve::Scalar::random());
        t.push_back(curve::Scalar::random());
        rho.push_back(curve::Scalar::random());

        // Fig. 2:  c_lj = Com(l_j, r_j);  c_aj = Com(a_j, s_j)
        const curve::Scalar l_jj = l_j[j] ? curve::Scalar::one() : curve::Scalar::zero();

        proof.C_l.push_back(curve::Commitment(l_jj, r_j.back()));
        proof.C_a.push_back(curve::Commitment(a.back(), s.back()));

        // Fig. 2:  c_bj = Com(l_j a_j, t_j), and l_j is a bit
        const curve::Scalar b_j = l_j[j] ? a.back() : curve::Scalar::zero();

        proof.C_b.push_back(curve::Commitment(b_j, t.back()));
    }

    // With a in hand, expand f_j,i_j to get p_i(x) -- equation (1), section 3:
    //
    //   p_i(x) = prod_{j=1..n} (delta_{i_j l_j} x + ...)
    //          = delta_{i,l} x^n + sum_{k=0..n-1} p_{i,k} x^k
    //
    // So p_i has degree n-1 for every i except l, where the leading x^n term
    // appears -- which is what the product equation in verify() detects.
    //
    // The citation used to read "section 2.3": that is the binary proof this
    // one calls n times, not the equation.  Equation (1) is in section 3.
    std::vector<Polynomial> p_x;
    for(std::size_t i = 0; i < N; i++) {
        Polynomial p_i_x;
        const Index i_j(i);

        for(std::size_t j = 0; j < n; j++) {
            const Polynomial f_j_i_j = i_j[j] ?
                (l_j[j] ? std::vector<curve::Scalar>{ a[j], curve::Scalar::one() } : std::vector<curve::Scalar>{ a[j] }) :
                (!l_j[j] ? std::vector<curve::Scalar>{ -a[j], curve::Scalar::one() } : std::vector<curve::Scalar>{ -a[j] });

            p_i_x = p_i_x * f_j_i_j;
        }

        p_x.push_back(p_i_x);
    }

    // And with every p_i(x), the C_d -- Fig. 2:
    //
    //   c_dk = prod_{i=0..N-1} c_i^{p_{i,k}} * Com(0, rho_k)
    //
    // Additively, a sum of scalar multiples.  The code's j is the paper's k.
    for(std::size_t j = 0; j < n; j++) {
        curve::Point C_d_k = curve::Point::zero();

        for(std::size_t i = 0; i < N; i++)
            C_d_k += proof.C[i] * p_x[i][j];

        C_d_k += curve::Commitment(curve::Scalar::zero(), rho[j]);

        proof.C_d.push_back(C_d_k);
    }

    // Every first-round commitment is now set, which is the point at which
    // the challenge may be taken and not before.
    const curve::Scalar x = challenge(proof, n);

    // Fig. 2:  f_j = l_j x + a_j;  z_aj = r_j x + s_j;  z_bj = r_j(x - f_j) + t_j
    for(std::size_t j = 0; j < n; j++) {
        const curve::Scalar f_j = l_j[j] ? (x + a[j]) : a[j];

        proof.f.push_back(f_j);
        proof.z_a.push_back(r_j[j] * x + s[j]);
        proof.z_b.push_back(r_j[j] * (x - f_j) + t[j]);
    }

    // Fig. 2:  z_d = r x^n - sum_{k=0..n-1} rho_k x^k
    proof.z_d = r * (x ^ int(n));
    for(std::size_t k = 0; k < n; k++)
        proof.z_d -= rho[k] * (x ^ int(k));

    return proof;
}

bool verify(const ZeroProof& proof) {
    const std::size_t n = index_bits(proof);

    if(n == 0)
        return false;

    const std::size_t N = proof.C.size();
    const curve::Scalar x = challenge(proof, n);

    // Each bit of the secret index is a binary proof in its own right -- the
    // paper's n parallel instances of section 2.3.  Fig. 2:
    //
    //   c_lj^x c_aj = Com(f_j, z_aj)
    //   c_lj^(x - f_j) c_bj = Com(0, z_bj)
    for(std::size_t j = 0; j < n; j++) {
        const curve::Point A_lhs = x * proof.C_l[j] + proof.C_a[j];
        const curve::Point A_rhs = curve::Commitment(proof.f[j], proof.z_a[j]);

        const curve::Point B_lhs = (x - proof.f[j]) * proof.C_l[j] + proof.C_b[j];
        const curve::Point B_rhs = curve::Commitment(curve::Scalar::zero(), proof.z_b[j]);

        if(A_lhs != A_rhs || B_lhs != B_rhs)
            return false;
    }

    // The last verification, Fig. 2:
    //
    //   prod_i c_i^{prod_j f_{j,i_j}} * prod_k c_dk^{-x^k} = Com(0, z_d)
    //
    // with f_{j,1} = f_j and f_{j,0} = x - f_j.  Built in two halves:
    // first prod_k c_dk^{-x^k}.
    curve::Point D = curve::Point::zero();
    for(std::size_t k = 0; k < n; k++)
        D += proof.C_d[k] * (-(x ^ int(k)));

    // then prod_i c_i^{prod_j f_{j,i_j}}.
    curve::Point P = curve::Point::zero();
    for(std::size_t i = 0; i < N; i++) {
        const Index i_j(i);

        curve::Scalar f_i = curve::Scalar::one();
        for(std::size_t j = 0; j < n; j++)
            f_i *= i_j[j] ? proof.f[j] : (x - proof.f[j]);

        P += proof.C[i] * f_i;
    }

    return (P + D) == curve::Commitment(curve::Scalar::zero(), proof.z_d);
}

}
}
}
