/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joey Yandle <dragon@dancingdragon.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 * 
 */

#include <ostream>

#include <sodium.h>
#include <sodium/crypto_core_ristretto255.h>

#include <jlib/crypt/groth.hh>
#include <jlib/util/util.hh>

namespace jlib {
namespace crypt {
namespace groth {

Scalar::Scalar() {
}

Scalar::Scalar(const Hash<Scalar::HASHBYTES>& hash) {
    crypto_core_ristretto255_scalar_reduce(reinterpret_cast<unsigned char*>(&m_bytes), reinterpret_cast<const unsigned char*>(&hash.m_bytes));
}
    
Scalar Scalar::random() {
    Scalar result;
    
    crypto_core_ristretto255_scalar_random(reinterpret_cast<unsigned char*>(&result.m_bytes));

    return result;
}

Scalar Scalar::zero() {
    Scalar result;

    std::memset(reinterpret_cast<unsigned char*>(&result.m_bytes), 0, crypto_core_ristretto255_SCALARBYTES);

    return result;
}

Scalar Scalar::one() {
    Scalar result = Scalar::zero();

    result.m_bytes[0] = 0x01;

    return result;
}

Scalar Scalar::operator+(const Scalar& x) const {
    Scalar result;

    crypto_core_ristretto255_scalar_add(reinterpret_cast<unsigned char*>(&result.m_bytes), reinterpret_cast<const unsigned char*>(&m_bytes), reinterpret_cast<const unsigned char*>(&x.m_bytes));

    return result;
}    

    
Scalar Scalar::operator*(const Scalar& x) const {
    Scalar result;

    crypto_core_ristretto255_scalar_mul(reinterpret_cast<unsigned char*>(&result.m_bytes), reinterpret_cast<const unsigned char*>(&m_bytes), reinterpret_cast<const unsigned char*>(&x.m_bytes));

    return result;
}

Point Scalar::operator*(const Point& x) const {
    return (x * *this);
}

Point::Point() {
}
    
Point::Point(const Scalar& scalar) {
    int e = crypto_scalarmult_ristretto255_base(reinterpret_cast<unsigned char*>(&m_bytes), reinterpret_cast<const unsigned char*>(&scalar.m_bytes));
    if(e != 0)
        throw std::runtime_error("crypto_scalarmult_ristretto255_base failed");
}
    
Point::Point(const Hash<Point::HASHBYTES>& hash) {
    crypto_core_ristretto255_from_hash(reinterpret_cast<unsigned char*>(&m_bytes), reinterpret_cast<const unsigned char*>(&hash.m_bytes));
}
    
Point Point::random() {
    Point result;
    Scalar exponent = Scalar::random();
    
    int e = crypto_scalarmult_ristretto255_base(reinterpret_cast<unsigned char*>(&result.m_bytes), reinterpret_cast<const unsigned char*>(&exponent.m_bytes));
    if(e != 0)
        throw std::runtime_error("crypto_scalarmult_ristretto255_base failed");

    return result;
}

Point Point::from(const Scalar& x) {
    return Point(x);
}

Point Point::operator+(const Point& x) const {
    Point result;

    int e = crypto_core_ristretto255_add(reinterpret_cast<unsigned char*>(&result.m_bytes), reinterpret_cast<const unsigned char*>(&m_bytes), reinterpret_cast<const unsigned char*>(&x.m_bytes));
    if(e != 0)
        throw std::runtime_error("crypto_core_ristretto255_add failed");
    return result;
}

Point Point::operator*(const Scalar& x) const {
    Point result;

    int e = crypto_scalarmult_ristretto255(reinterpret_cast<unsigned char*>(&result.m_bytes), reinterpret_cast<const unsigned char*>(&x.m_bytes), reinterpret_cast<const unsigned char*>(&m_bytes));
    if(e != 0)
        throw std::runtime_error("crypto_scalarmult_ristretto255 failed");
    return result;
}

BasePoint::BasePoint() {
    Scalar one = Scalar::one();

    int e = crypto_scalarmult_ristretto255_base(reinterpret_cast<unsigned char*>(&m_bytes), reinterpret_cast<const unsigned char*>(&one.m_bytes));
    if(e != 0)
        throw std::runtime_error("crypto_scalarmult_ristretto255_base failed");
}

Point BasePoint::operator*(const Scalar& x) const {
    Point result;

    int e = crypto_scalarmult_ristretto255_base(reinterpret_cast<unsigned char*>(&result.m_bytes), reinterpret_cast<const unsigned char*>(&x.m_bytes));
    if(e != 0)
        throw std::runtime_error("crypto_scalarmult_ristretto255_base failed");
    return result;
}
    
std::ostream& operator<<(std::ostream& out, const Scalar& d) {
    out << util::hex_value(reinterpret_cast<const unsigned char*>(&d.m_bytes), crypto_core_ristretto255_SCALARBYTES);

    return out;
}

std::ostream& operator<<(std::ostream& out, const Point& d) {
    out << util::hex_value(reinterpret_cast<const unsigned char*>(&d.m_bytes), crypto_core_ristretto255_BYTES);

    return out;
}
    
class BinaryProver {
    
};

class BinaryVerifier {


};
    
class Proof {
    
};

    
}
}
}
