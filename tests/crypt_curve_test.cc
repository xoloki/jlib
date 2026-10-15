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

#include <jlib/crypt/curve.hh>

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace jlib::crypt::curve;

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

/** Leave a recognisable pattern on the stack for the next frame to inherit. */
static void dirty_stack() __attribute__((noinline));
static void dirty_stack() {
    volatile unsigned char junk[512];

    for(int i = 0; i < 512; i++)
        junk[i] = static_cast<unsigned char>(0xa5 + i);
}

static bool all_zero(const unsigned char* p, std::size_t n) {
    for(std::size_t i = 0; i < n; i++)
        if(p[i] != 0)
            return false;

    return true;
}

/** The ristretto255 group order L, little-endian. */
static const unsigned char L[Scalar::SIZE] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

int main(int argc, char** argv) {
    // libsodium requires this before any other call into it.
    if(sodium_init() < 0) {
        std::cerr << "sodium_init() failed" << std::endl;
        return -1;
    }

    std::cout << "curve::Scalar\n";
    {
        // #417: the default constructor left m_data as whatever was on the
        // stack, so a Scalar that had not been assigned was a usable secret
        // nobody chose.  Best-effort: the check depends on the compiler
        // reusing the frame dirty_stack() just wrote, which it need not do --
        // so a pass is weak evidence and a failure is conclusive.
        bool zeroed = true;
        for(int i = 0; i < 8 && zeroed; i++) {
            dirty_stack();
            Scalar s;
            zeroed = all_zero(s.data(), Scalar::SIZE);
        }
        ok("#417 a default-constructed Scalar is zeroed, over eight tries", zeroed);

        ok("Scalar(0) is zero", Scalar(std::size_t(0)) == Scalar::zero());

        // The size_t constructor used to memcpy the object representation of
        // x, which agrees with the little-endian scalar encoding only on a
        // little-endian host.
        ok("Scalar(1) is one", Scalar(std::size_t(1)) == Scalar::one());
        ok("and Scalar(2) is one + one",
           Scalar(std::size_t(2)) == Scalar::one() + Scalar::one());

        const Scalar x = Scalar::random();
        const Scalar y = Scalar::random();

        ok("two random scalars differ", x != y);
        ok("x + 0 == x", x + Scalar::zero() == x);
        ok("x - x == 0", x - x == Scalar::zero());
        ok("x * 1 == x", x * Scalar::one() == x);
        ok("x * 0 == 0", x * Scalar::zero() == Scalar::zero());
        ok("addition commutes", x + y == y + x);
        ok("-x + x == 0", -x + x == Scalar::zero());
        ok("x * x^-1 == 1", x * (x^-1) == Scalar::one());
        ok("x^0 == 1", (x^0) == Scalar::one());
        ok("x^1 == x", (x^1) == x);
        ok("x^2 == x*x", (x^2) == x*x);
        ok("x^-2 == (x*x)^-1", (x^-2) == ((x*x)^-1));

        // #418: invert() returns -1 for a scalar with no inverse, and the old
        // code returned the output buffer regardless -- so inverting zero
        // yielded a valid-looking zero and the caller carried on with the
        // blinding removed.
        ok("#418 inverting zero throws rather than returning zero",
           throws([] { return Scalar::zero() ^ -1; }));
    }

    std::cout << "\ncurve::Scalar::from_bytes\n";
    {
        const Scalar x = Scalar::random();
        ok("#422 a scalar round-trips through its encoding",
           Scalar::from_bytes(x.data()) == x);

        unsigned char zero[Scalar::SIZE] = { 0 };
        ok("#422 the zero encoding is canonical and accepted",
           Scalar::from_bytes(zero) == Scalar::zero());

        unsigned char below[Scalar::SIZE];
        std::memcpy(below, L, Scalar::SIZE);
        below[0] -= 1;
        ok("#422 L-1 is the largest canonical scalar and is accepted",
           !throws([&below] { return Scalar::from_bytes(below); }));

        ok("#422 L itself is rejected", throws([] { return Scalar::from_bytes(L); }));

        unsigned char ones[Scalar::SIZE];
        std::memset(ones, 0xff, Scalar::SIZE);
        ok("#422 an all-ones encoding is rejected",
           throws([&ones] { return Scalar::from_bytes(ones); }));

        unsigned char high[Scalar::SIZE];
        std::memcpy(high, L, Scalar::SIZE);
        high[Scalar::SIZE - 1] |= 0x80;
        ok("#422 a scalar with the top bit set is rejected",
           throws([&high] { return Scalar::from_bytes(high); }));
    }

    std::cout << "\ncurve::Point\n";
    {
        const BasePoint G;
        const Scalar x = Scalar::random();
        const Scalar y = Scalar::random();

        ok("#417 a default-constructed Point is zeroed",
           all_zero(Point().data(), Point::SIZE));

        ok("Point::from(x) is x * G", Point::from(x) == x * G);
        ok("0 + P == P", Point::zero() + Point::from(x) == Point::from(x));
        ok("P - P == 0", Point::from(x) - Point::from(x) == Point::zero());
        ok("(x + y) * G == x*G + y*G", Point::from(x + y) == x * G + y * G);
        ok("P * 1 == P", Point::from(x) * Scalar::one() == Point::from(x));
        ok("two random points differ", Point::random() != Point::random());
    }

    std::cout << "\ncurve::Point::from_bytes\n";
    {
        const Point P = Point::random();
        ok("#422 a point round-trips through its encoding", Point::from_bytes(P.data()) == P);

        unsigned char identity[Point::SIZE] = { 0 };
        ok("#422 the all-zero encoding is the identity and is accepted",
           Point::from_bytes(identity) == Point::zero());

        // A ristretto255 encoding is a canonical field element, so bit 255 is
        // always clear and setting it is rejected for every point rather than
        // for most of them.
        //
        // from_bytes checks this itself rather than leaving it to libsodium,
        // which is what makes the assertion portable: libsodium 1.0.18 masks
        // bit 255 before checking canonicity, so it accepts a forced bit 255
        // for every point where 1.0.22 rejects it.  This failed in a container
        // and passed on the host until the canonicity test moved into
        // from_bytes.
        unsigned char high[Point::SIZE];
        std::memcpy(high, P.data(), Point::SIZE);
        high[Point::SIZE - 1] |= 0x80;
        ok("#422 an encoding with the top bit set is rejected",
           throws([&high] { return Point::from_bytes(high); }));

        // Non-canonical with the top bit clear: the field prime itself.  Both
        // libsodium versions measured reject this one on their own -- 1.0.18's
        // divergence is only the masked bit above -- so this documents the
        // boundary rather than guarding from_bytes's own check.
        unsigned char prime[Point::SIZE] = {
            0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f };
        ok("#422 the field prime is not a canonical field element and is rejected",
           throws([&prime] { return Point::from_bytes(prime); }));

        unsigned char ones[Point::SIZE];
        std::memset(ones, 0xff, Point::SIZE);
        ok("#422 an all-ones encoding is rejected",
           throws([&ones] { return Point::from_bytes(ones); }));
    }

    std::cout << "\ncurve::Commitment\n";
    {
        const BasePoint G;

        ok("H is not G", Commitment::H() != static_cast<const Point&>(G));
        ok("and H is the same point every call", &Commitment::H() == &Commitment::H());

        const Scalar v1 = Scalar::random(), b1 = Scalar::random();
        const Scalar v2 = Scalar::random(), b2 = Scalar::random();

        ok("Com(v, b) is v*G + b*H",
           static_cast<const Point&>(Commitment(v1, b1)) == v1 * G + b1 * Commitment::H());

        // The homomorphism is the whole point of a Pedersen commitment, and
        // it is what groth's proofs are built on.
        ok("Com(v1,b1) + Com(v2,b2) == Com(v1+v2, b1+b2)",
           Commitment(v1, b1) + Commitment(v2, b2) ==
           static_cast<const Point&>(Commitment(v1 + v2, b1 + b2)));

        // Hiding: the same value twice must not give the same commitment,
        // which is exactly what the random blinding factor is for.
        ok("committing to one value twice gives two different commitments",
           static_cast<const Point&>(Commitment(v1)) !=
           static_cast<const Point&>(Commitment(v1)));
    }

    std::cout << "\ncurve::Hash\n";
    {
        const BasePoint G;
        const Hash<Point::HASHSIZE> h = hash<Point::HASHSIZE>(G);

        ok("hashing the same input twice gives the same point",
           Point(h) == Point(hash<Point::HASHSIZE>(G)));
        ok("and a different input gives a different point",
           Point(h) != Point(hash<Point::HASHSIZE>(Point::random())));

        // Scalar::HASHSIZE and Point::HASHSIZE are both
        // crypto_core_ristretto255_HASHBYTES, so one Hash satisfies both
        // constructors -- as a Scalar it is reduced mod the group order, as a
        // Point it goes through crypto_core_ristretto255_from_hash.  Both are
        // right and they are not the same thing, which is why neither
        // conversion is implicit any more.
        ok("the same hash as a scalar and as a point are unrelated",
           Point::from(Scalar(h)) != Point(h));

        // H has to be a point whose discrete log with respect to G nobody
        // knows: that is all Pedersen binding rests on.  It therefore has to
        // come from a real map to the group, and must not be hash-to-scalar
        // followed by a base multiply, which would hand the dlog to anyone
        // who recomputed the hash.
        ok("H is hash-to-curve, not hash-to-scalar times G",
           Commitment::H() == Point(hash<Point::HASHSIZE>(static_cast<const Point&>(G))) &&
           Commitment::H() != Point::from(Scalar(hash<Scalar::HASHSIZE>(static_cast<const Point&>(G)))));
    }

    // Deliberately not tested here:
    //
    // That ~Scalar() scrubs its bytes.  It does, with sodium_memzero, but
    // reading the storage of a destroyed object to check is undefined
    // behaviour, and a test that performs it is not evidence about anything.
    // The guarantee is in the destructor and in curve.hh.
    //
    // Nor that the comparisons are constant-time.  They use sodium_memcmp,
    // which is; timing that from a unit test would measure the host rather
    // than the code, and a threshold on it would key on the host's load.
    //
    // Nor Scalar::random()'s distribution, which is libsodium's business --
    // crypto_core_ristretto255_scalar_random -- and not something a hundred
    // samples can say anything about.

    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
