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

#include <ostream>

#include <sodium.h>
#include <sodium/crypto_core_ristretto255.h>

#include <jlib/crypt/curve.hh>
#include <jlib/util/util.hh>

namespace jlib {
namespace crypt {
namespace curve {

Scalar::Scalar() {
    // Zeroed, not left as whatever was on the stack.  Every arithmetic path
    // overwrites the whole buffer so this was invisible there, but `Scalar s;`
    // is public API and a scalar is a key, a nonce or a blinding factor.
    sodium_memzero(m_data, sizeof(m_data));
}

Scalar::~Scalar() {
    // A secret should not outlive its scope in memory.  schnorr's nonce is the
    // case that matters: `s = k - c*x` is published, so recovering k recovers
    // the private x.
    sodium_memzero(m_data, sizeof(m_data));
}

Scalar::Scalar(std::size_t x) {
    sodium_memzero(m_data, sizeof(m_data));

    // Little-endian, by shifting rather than by copying the bytes of x: a
    // memcpy of the object representation agrees with the ristretto255
    // encoding only on a little-endian host.
    static_assert(sizeof(x) <= crypto_core_ristretto255_SCALARBYTES,
                  "std::size_t does not fit in a ristretto255 scalar");
    for(std::size_t i = 0; i < sizeof(x); i++)
        m_data[i] = static_cast<unsigned char>(x >> (8 * i));
}

Scalar::Scalar(const Hash<Scalar::HASHSIZE>& hash) {
    crypto_core_ristretto255_scalar_reduce(reinterpret_cast<unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&hash.m_data));
}
    
Scalar Scalar::from_bytes(const unsigned char* b) {
    // The ristretto255 group order L, little-endian.  A scalar is canonical
    // only if it is numerically below this; sodium_compare does that
    // comparison in constant time and treats both operands as little-endian.
    static const unsigned char L[crypto_core_ristretto255_SCALARBYTES] = {
        0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
        0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
    };

    if(sodium_compare(b, L, crypto_core_ristretto255_SCALARBYTES) >= 0)
        throw std::runtime_error("curve::Scalar::from_bytes: not a canonical scalar");

    Scalar result;
    std::memcpy(result.m_data, b, crypto_core_ristretto255_SCALARBYTES);

    return result;
}

Scalar Scalar::random() {
    Scalar result;
    
    crypto_core_ristretto255_scalar_random(reinterpret_cast<unsigned char*>(&result.m_data));

    return result;
}

Scalar Scalar::zero() {
    Scalar result;

    std::memset(reinterpret_cast<unsigned char*>(&result.m_data), 0, crypto_core_ristretto255_SCALARBYTES);

    return result;
}

Scalar Scalar::one() {
    Scalar result = Scalar::zero();

    result.m_data[0] = 0x01;

    return result;
}

const unsigned char* Scalar::data() const {
    return reinterpret_cast<const unsigned char*>(m_data);
}
    
    
Scalar Scalar::operator+(const Scalar& x) const {
    Scalar result;

    crypto_core_ristretto255_scalar_add(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&x.m_data));

    return result;
}    

Scalar Scalar::operator-(const Scalar& x) const {
    Scalar result;

    crypto_core_ristretto255_scalar_sub(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&x.m_data));

    return result;
}    

Scalar Scalar::operator*(const Scalar& x) const {
    Scalar result;

    crypto_core_ristretto255_scalar_mul(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&x.m_data));

    return result;
}

Scalar Scalar::operator^(int k) const { 
    if(k == -1) {
        Scalar result;

        // Checked, like every other libsodium call in this file.  invert()
        // returns -1 for a scalar with no inverse, which in a prime field means
        // zero -- and the old code returned the output buffer regardless, so
        // inverting zero yielded a valid-looking zero and the protocol carried
        // on with the blinding removed.
        int e = crypto_core_ristretto255_scalar_invert(result.m_data, m_data);
        if(e != 0)
            throw std::runtime_error("curve::Scalar::operator^(-1): scalar has no inverse");

        return result;
    }

    curve::Scalar a_k = curve::Scalar::one();

    unsigned int j = std::abs(k);
    
    for(unsigned int i = 0; i < j; i++) {
        a_k *= (*this);
    }

    if(static_cast<int>(j) != k) {
        return a_k^(-1);
    } else {
        return a_k;
    }
}
    
Point Scalar::operator*(const Point& x) const {
    return (x * *this);
}

Point Scalar::operator*(const BasePoint& x) const {
    return (x * *this);
}

Scalar Scalar::operator-() const {
    Scalar result;

    crypto_core_ristretto255_scalar_negate(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&m_data));

    return result;
}

Scalar& Scalar::operator+=(const Scalar& x) {
    Scalar result;

    crypto_core_ristretto255_scalar_add(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&x.m_data));

    *this = result;

    return *this;
}    

Scalar& Scalar::operator-=(const Scalar& x) {
    Scalar result;

    crypto_core_ristretto255_scalar_sub(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&x.m_data));

    *this = result;

    return *this;
}    

Scalar& Scalar::operator*=(const Scalar& x) {
    Scalar result;

    crypto_core_ristretto255_scalar_mul(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&x.m_data));

    *this = result;

    return *this;
}    

Point::Point() {
    sodium_memzero(m_data, sizeof(m_data));
}
    
Point::Point(const Scalar& scalar) {
    int e = crypto_scalarmult_ristretto255_base(reinterpret_cast<unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&scalar.m_data));
    if(e != 0)
        throw std::runtime_error("crypto_scalarmult_ristretto255_base failed");
}
    
Point::Point(const Hash<Point::HASHSIZE>& hash) {
    crypto_core_ristretto255_from_hash(reinterpret_cast<unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&hash.m_data));
}
    
Point Point::from_bytes(const unsigned char* b) {
    // The ristretto255 field prime p = 2^255 - 19, little-endian.  A
    // ristretto255 encoding is a canonical field element or it is not an
    // encoding at all, and that is checked here rather than left to libsodium
    // deliberately: libsodium 1.0.18 accepts every encoding with bit 255 set
    // -- measured, 200 of 200 random points stayed "valid" with the bit forced
    // on -- where 1.0.22 rejects them.
    //
    // Two byte strings decoding to one point is malleability, and these
    // encodings go straight into the Fiat-Shamir challenge in schnorr and
    // groth, where Hash::update takes the stored bytes.  So the guarantee has
    // to be this library's and not the installed libsodium's, the same way
    // Scalar::from_bytes checks against the group order itself.
    static const unsigned char P[crypto_core_ristretto255_BYTES] = {
        0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f };

    if(sodium_compare(b, P, crypto_core_ristretto255_BYTES) >= 0)
        throw std::runtime_error("curve::Point::from_bytes: not a canonical field element");

    if(crypto_core_ristretto255_is_valid_point(b) != 1)
        throw std::runtime_error("curve::Point::from_bytes: not a ristretto255 point");

    Point result;
    std::memcpy(result.m_data, b, crypto_core_ristretto255_BYTES);

    return result;
}

Point Point::zero() {
    Point result;

    std::memset(reinterpret_cast<unsigned char*>(&result.m_data), 0, crypto_core_ristretto255_BYTES);

    return result;
}

Point Point::random() {
    Point result;
    
    crypto_core_ristretto255_random(reinterpret_cast<unsigned char*>(&result.m_data));

    return result;
}

Point Point::from(const Scalar& x) {
    return Point(x);
}

const unsigned char* Point::data() const {
    return reinterpret_cast<const unsigned char*>(m_data);
}
    

Point Point::operator+(const Point& x) const {
    Point result;

    int e = crypto_core_ristretto255_add(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&x.m_data));
    if(e != 0)
        throw std::runtime_error("crypto_core_ristretto255_add failed");
    return result;
}

Point Point::operator-(const Point& x) const {
    Point result;

    int e = crypto_core_ristretto255_sub(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&x.m_data));
    if(e != 0)
        throw std::runtime_error("crypto_core_ristretto255_sub failed");
    return result;
}

Point Point::operator*(const Scalar& x) const {
    Point result;

    int e = crypto_scalarmult_ristretto255(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&x.m_data), reinterpret_cast<const unsigned char*>(&m_data));

    // scalarmult returns -1 either for an invalid input point or for a result
    // that is the identity.  Every Point is a valid ristretto255 encoding by
    // construction -- from_bytes validates, and every other way of making one
    // goes through a libsodium operation that produces a valid point -- so the
    // first case cannot arise here and -1 means the identity.
    //
    // The `&& x == Scalar::zero()` that used to be part of this condition
    // decided the branch on whether a possibly-secret scalar was zero.  It was
    // also redundant: libsodium has already told us what the answer is.
    if(e == -1)
        return Point::zero();

    if(e != 0)
        throw std::runtime_error("crypto_scalarmult_ristretto255 failed");
    
    return result;
}

Point& Point::operator+=(const Point& x) {
    Point result;

    int e = crypto_core_ristretto255_add(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&x.m_data));
    if(e != 0)
        throw std::runtime_error("crypto_core_ristretto255_add failed");

    *this = result;
    
    return *this;
}

BasePoint::BasePoint() {
    Scalar one = Scalar::one();

    int e = crypto_scalarmult_ristretto255_base(reinterpret_cast<unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&one.m_data));
    if(e != 0)
        throw std::runtime_error("crypto_scalarmult_ristretto255_base failed");
}

Point BasePoint::operator*(const Scalar& x) const {
    Point result;

    int e = crypto_scalarmult_ristretto255_base(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&x.m_data));
    // As in Point::operator*: -1 is the identity result, and the scalar
    // comparison that used to be in this condition branched on a secret.
    if(e == -1)
        return Point::zero();

    if(e != 0)
        throw std::runtime_error("crypto_scalarmult_ristretto255_base failed");
    return result;
}
    
std::ostream& operator<<(std::ostream& out, const Scalar& d) {
    out << util::hex_value(reinterpret_cast<const unsigned char*>(&d.m_data), crypto_core_ristretto255_SCALARBYTES);

    return out;
}

std::ostream& operator<<(std::ostream& out, const Point& d) {
    out << util::hex_value(reinterpret_cast<const unsigned char*>(&d.m_data), crypto_core_ristretto255_BYTES);

    return out;
}

bool operator==(const Scalar& x, const Scalar& y) {
    // sodium_memcmp, not memcmp: a Scalar is secret, and memcmp returns at the
    // first differing byte, which makes comparing one against an
    // attacker-influenced value a prefix oracle.
    return (sodium_memcmp(x.m_data, y.m_data, crypto_core_ristretto255_SCALARBYTES) == 0);
}
    
bool operator!=(const Scalar& x, const Scalar& y) {
    return !(x == y);
}
    
bool operator==(const Point& x, const Point& y) {
    // A point is public, so this one is not a leak -- but using the same
    // primitive for both means nobody has to work out which is which.
    return (sodium_memcmp(x.m_data, y.m_data, crypto_core_ristretto255_BYTES) == 0);
}

bool operator!=(const Point& x, const Point& y) {
    return !(x == y);
}

const BasePoint& Commitment::G() {
    static const BasePoint g;
    return g;
}

const Point& Commitment::H() {
    // Hash-to-curve, via crypto_core_ristretto255_from_hash.  H has to be a
    // point whose discrete log with respect to G nobody knows -- that is the
    // whole of Pedersen binding -- so it must come from a real map to the
    // group and never from hash-to-scalar followed by a base multiply.  The
    // cast now says which of the two this is.
    static const Point h(hash<Point::HASHSIZE>(G()));
    return h;
}
    
Commitment::Commitment() {
}
    
Commitment::Commitment(const Scalar& value)
    : Commitment(value, Scalar::random())
{
}

Commitment::Commitment(const Scalar& value, const Scalar& blind)
    : m_value(value),
      m_blind(blind)
{
    Point p = value * G() + blind * H();
    static_cast<Point&>(*this) = p;
}

}
}
}
