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

#include <jlib/crypt/groth.hh>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace jlib::crypt;
using jlib::crypt::curve::Commitment;
using jlib::crypt::curve::Point;
using jlib::crypt::curve::Scalar;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

template<typename F>
static bool throws(F f) {
    try {
        f();
    } catch(const std::exception&) {
        return true;
    }
    return false;
}

/**
 * A list of commitments to random values, except at zero_at, which commits to
 * zero with the blinding factor returned through blind -- which is what a
 * ZeroProof needs to know.
 */
static std::vector<Commitment> ring(std::size_t count, std::size_t zero_at, Scalar& blind) {
    std::vector<Commitment> C;

    for(std::size_t i = 0; i < count; i++) {
        if(i == zero_at) {
            blind = Scalar::random();
            C.push_back(Commitment(Scalar::zero(), blind));
        } else {
            C.push_back(Commitment(Scalar::random()));
        }
    }

    return C;
}

int main(int argc, char** argv) {
    // libsodium requires this before any other call into it.
    if(sodium_init() < 0) {
        std::cerr << "sodium_init() failed" << std::endl;
        return -1;
    }

    std::cout << "groth::BinaryProof\n";
    {
        const Scalar r = Scalar::random();

        ok("a commitment to zero proves binary", groth::verify(groth::prove(Scalar::zero(), r)));
        ok("a commitment to one proves binary", groth::verify(groth::prove(Scalar::one(), r)));

        // prove() used to return a proof that simply failed to verify.  The
        // prover knows m, so this is a mistake in the calling code, and
        // handing back an unverifiable proof buries it.
        ok("a value that is not a bit is refused rather than proven",
           throws([&r] { groth::prove(Scalar::random(), r); }));

        groth::BinaryProof p = groth::prove(Scalar::one(), r);

        groth::BinaryProof bad_f = p;
        bad_f.f = Scalar::random();
        ok("a changed f does not verify", !groth::verify(bad_f));

        groth::BinaryProof bad_z = p;
        bad_z.z_b = Scalar::random();
        ok("a changed z_b does not verify", !groth::verify(bad_z));

        // The challenge covers all three commitments, so swapping the
        // statement out from under the responses changes x as well.
        groth::BinaryProof bad_C = p;
        bad_C.C = Commitment(Scalar::one(), Scalar::random());
        ok("the proof does not transfer to another commitment to the same bit",
           !groth::verify(bad_C));
    }

    std::cout << "\ngroth::ZeroProof\n";
    {
        Scalar blind;
        const std::vector<Commitment> C = ring(8, 5, blind);

        ok("one of eight commitments is proven zero", groth::verify(groth::prove(C, 5, blind)));

        // 6 is not a power of two, so prove() pads to 8 by repeating the last
        // commitment; the proof's own C is longer than the one passed in.
        Scalar pad_blind;
        const std::vector<Commitment> six = ring(6, 1, pad_blind);
        const groth::ZeroProof padded = groth::prove(six, 1, pad_blind);
        ok("a list that is not a power of two is padded up", padded.C.size() == 8,
           "C is " + std::to_string(padded.C.size()));
        ok("and the padded proof verifies", groth::verify(padded));

        // Nothing in the list opens to zero, so there is nothing to prove.
        // prove() cannot tell -- it is handed r and trusts it -- but the
        // proof it builds must not verify.
        std::vector<Commitment> none;
        for(std::size_t i = 0; i < 4; i++)
            none.push_back(Commitment(Scalar::random()));
        ok("a list with no zero commitment yields a proof that does not verify",
           !groth::verify(groth::prove(none, 0, Scalar::random())));

        Scalar wrong_blind;
        const std::vector<Commitment> four = ring(4, 3, wrong_blind);
        ok("naming the wrong index does not verify",
           !groth::verify(groth::prove(four, 0, wrong_blind)));
        ok("and the right index with the wrong blinding factor does not verify",
           !groth::verify(groth::prove(four, 3, Scalar::random())));

        ok("one commitment hides nothing among the others and is refused",
           throws([&four] { groth::prove(std::vector<Commitment>{four[0]}, 0, Scalar::random()); }));
        ok("an empty list is refused",
           throws([] { groth::prove(std::vector<Commitment>{}, 0, Scalar::random()); }));
        ok("an index outside the list is refused",
           throws([&four, &wrong_blind] { groth::prove(four, 4, wrong_blind); }));
    }

    std::cout << "\n#414 a proof's vector lengths are as untrusted as its contents\n";
    {
        // verify() reads n out of the proof and then indexes seven vectors
        // with it.  It used to take n from c_a alone and index the rest
        // unchecked, so any proof whose vectors disagreed read past the end
        // of a std::vector -- reachable by anyone who can hand us a proof.
        //
        // What these assertions pin is the contract: a proof of the wrong
        // shape is rejected.  They do not, on their own, catch the
        // out-of-bounds read.  Checked by sabotage: with the shape check
        // removed, eleven of the thirteen still passed, because reading one
        // element past a vector's size usually lands inside its capacity and
        // returns a wrong answer rather than crashing -- so verify() returned
        // false anyway and the test went green against the code it is meant
        // to catch.  The read is only visible with the container annotations
        // on:
        //
        //   clang++ -fsanitize=address tests/crypt_groth_test.cc \
        //       jlib/crypt/groth.cc jlib/crypt/curve.cc ...
        //
        // which on the unchecked version aborts with
        // "container-overflow ... READ of size 32" inside Commitment's
        // constructor, and on this one exits clean.
        Scalar blind;
        const std::vector<Commitment> C = ring(4, 2, blind);
        const groth::ZeroProof good = groth::prove(C, 2, blind);

        ok("the proof these are derived from verifies", groth::verify(good));

        struct {
            const char* what;
            void (*break_it)(groth::ZeroProof&);
        } cases[] = {
            { "a short f",     [](groth::ZeroProof& p) { p.f.pop_back(); } },
            { "a short z_a",   [](groth::ZeroProof& p) { p.z_a.pop_back(); } },
            { "a short z_b",   [](groth::ZeroProof& p) { p.z_b.pop_back(); } },
            { "a short C_a",   [](groth::ZeroProof& p) { p.C_a.pop_back(); } },
            { "a short C_b",   [](groth::ZeroProof& p) { p.C_b.pop_back(); } },
            { "a short C_d",   [](groth::ZeroProof& p) { p.C_d.pop_back(); } },
            { "a short C_l",   [](groth::ZeroProof& p) { p.C_l.pop_back(); } },
            { "a short C",     [](groth::ZeroProof& p) { p.C.pop_back(); } },
            { "a long f",      [](groth::ZeroProof& p) { p.f.push_back(Scalar::random()); } },
            { "a long C_d",    [](groth::ZeroProof& p) { p.C_d.push_back(Point::random()); } },
            { "a long C",      [](groth::ZeroProof& p) { p.C.push_back(Commitment(Scalar::random())); } },
        };

        for(const auto& c : cases) {
            groth::ZeroProof p = good;
            c.break_it(p);
            ok(std::string("#414 ") + c.what + " is rejected", !groth::verify(p));
        }

        ok("#414 an empty proof is rejected", !groth::verify(groth::ZeroProof()));

        // C must hold exactly 2^n commitments, so a list whose length is not
        // a power of two cannot be the one the bit proofs are about.
        groth::ZeroProof not_pow2 = good;
        not_pow2.C.resize(3, C.back());
        ok("#414 a commitment count that is not 2^n is rejected", !groth::verify(not_pow2));
    }

    // Deliberately not tested here:
    //
    // That the challenge binds C_l.  It does now -- prove() and verify() both
    // hash it, where neither used to -- and that is the standard requirement
    // on a Fiat-Shamir transform: a first-round commitment left out of the
    // challenge is one the prover may still choose after seeing it, and both
    // bit equations are linear in C_l, so a prover who can pick it afterwards
    // can solve for one that passes the bit check while committing to
    // something that is not a bit.  But no assertion here can tell the bound
    // version from the unbound one: tampering with C_l fails the bit
    // equations either way, so such a check would pass against the code it is
    // meant to catch.  Demonstrating it needs a forgery built against the old
    // challenge, which is a separate exercise and not a regression test.  The
    // property is argued in groth.hh instead.
    //
    // Nor the timing of prove()/verify().  The old test printed both; a
    // number with nothing to compare it against is not an assertion, and
    // asserting one would key on the host's load rather than on this code.

    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
