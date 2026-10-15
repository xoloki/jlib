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

#include <jlib/crypt/schnorr.hh>

#include <iostream>
#include <string>

using namespace jlib::crypt;
using jlib::crypt::curve::BasePoint;
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

int main(int argc, char** argv) {
    // libsodium requires this before any other call into it.
    if(sodium_init() < 0) {
        std::cerr << "sodium_init() failed" << std::endl;
        return -1;
    }

    const Point G = BasePoint();

    std::cout << "schnorr::Proof, no message\n";
    {
        const Scalar x = Scalar::random();
        const Point Y = x * G;
        const schnorr::Proof p = schnorr::prove(G, Y, x);

        ok("a proof of knowledge verifies", schnorr::verify(p, G, Y));

        // The old API kept the statement inside the proof, so verify() checked
        // a proof against the very key it carried and could not be handed a
        // different one.  This is what that made inexpressible.
        const Scalar other_x = Scalar::random();
        ok("#421 a proof for one key does not verify against another",
           !schnorr::verify(p, G, other_x * G));

        schnorr::Proof bad_s = p;
        bad_s.s = Scalar::random();
        ok("a changed s does not verify", !schnorr::verify(bad_s, G, Y));

        schnorr::Proof bad_R = p;
        bad_R.R = Point::random();
        ok("a changed R does not verify", !schnorr::verify(bad_R, G, Y));

        // G is in the challenge, so it cannot be swapped after the fact.
        ok("#421 the proof does not verify under a substituted generator",
           !schnorr::verify(p, Commitment::H(), Y));

        ok("two proofs of the same statement differ", schnorr::prove(G, Y, x).R != p.R,
           "R is the commitment to a fresh nonce and must never repeat");
    }

    std::cout << "\nschnorr::Proof, with a message\n";
    {
        const Scalar x = Scalar::random();
        const Point Y = x * G;
        const std::string m = "transfer 1 unit to alice";
        const schnorr::Proof p = schnorr::prove(G, Y, x, m);

        ok("a proof over a message verifies against that message",
           schnorr::verify(p, G, Y, m));
        ok("and not against a different message",
           !schnorr::verify(p, G, Y, std::string("transfer 1 unit to bob")));
        ok("and not against no message at all", !schnorr::verify(p, G, Y));

        // The converse: a bare proof of knowledge is not a signature, and
        // must not be accepted as one.
        const schnorr::Proof bare = schnorr::prove(G, Y, x);
        ok("a proof with no message does not verify against one",
           !schnorr::verify(bare, G, Y, m));

        ok("an empty message is still a message, distinct from none",
           schnorr::verify(schnorr::prove(G, Y, x, std::string()), G, Y, std::string()) &&
           !schnorr::verify(schnorr::prove(G, Y, x, std::string()), G, Y));
    }

    std::cout << "\nschnorr::MultiProof\n";
    {
        const schnorr::Generators<3>& Gs = schnorr::generators<3>();

        ok("the generator chain starts at the base point", Gs[0] == G);
        ok("and its generators are distinct",
           Gs[0] != Gs[1] && Gs[1] != Gs[2] && Gs[0] != Gs[2]);
        ok("and it is the same chain every time", &schnorr::generators<3>() == &Gs,
           "built once, not rebuilt per proof as it used to be");

        Scalar x[3] = { Scalar::random(), Scalar::random(), Scalar::random() };
        Point Y = Point::zero();
        for(int i = 0; i < 3; i++)
            Y += Gs[i] * x[i];

        const schnorr::MultiProof<3> p = schnorr::prove(Gs, Y, x);
        ok("a three-generator proof verifies", schnorr::verify(p, Gs, Y));

        ok("#421 it does not verify against another statement",
           !schnorr::verify(p, Gs, Y + G));

        schnorr::MultiProof<3> bad = p;
        bad.s[1] = Scalar::random();
        ok("a changed response does not verify", !schnorr::verify(bad, Gs, Y));

        // Every generator is in the challenge, so none can be substituted --
        // the bug the old GeneralProof<N> had, which hashed none of them.
        schnorr::Generators<3> swapped = Gs;
        swapped[2] = Point::random();
        ok("#421 MultiProof rejects a substituted generator",
           !schnorr::verify(p, swapped, Y));

        const std::string m = "three secrets";
        const schnorr::MultiProof<3> signed_p = schnorr::prove(Gs, Y, x, m);
        ok("a MultiProof over a message verifies against it",
           schnorr::verify(signed_p, Gs, Y, m));
        ok("#421 MultiProof rejects a changed message",
           !schnorr::verify(signed_p, Gs, Y, std::string("four secrets")));
    }

    std::cout << "\nschnorr::MultiProof, N = 1\n";
    {
        // N=1 is the same scheme as Proof, over the chain's first generator,
        // and is worth pinning because DoubleProof used to be a hand-written
        // copy of the N=2 case rather than an instance of one scheme.
        const schnorr::Generators<1>& Gs = schnorr::generators<1>();
        Scalar x[1] = { Scalar::random() };
        const Point Y = Gs[0] * x[0];

        ok("a one-generator MultiProof verifies",
           schnorr::verify(schnorr::prove(Gs, Y, x), Gs, Y));
    }

    // Deliberately not tested here:
    //
    // That a proof is zero-knowledge, which no unit test can show -- it is a
    // property of the simulator, not of any run.  What is tested is that the
    // nonce commitment differs between two proofs of one statement, which is
    // the part whose failure would leak x outright.
    //
    // Nor malleability of the encoding: Proof holds a Point and a Scalar, and
    // curve::Point::from_bytes and Scalar::from_bytes are where a hostile
    // encoding is rejected.  crypt_curve_test covers those.

    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
