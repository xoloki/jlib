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

#include <jlib/crypt/curve.hh>
#include <jlib/util/util.hh>

namespace jlib {
namespace crypt {
namespace curve {

Scalar::Scalar() {
}

Scalar::Scalar(const Hash<Scalar::HASHSIZE>& hash) {
    crypto_core_ristretto255_scalar_reduce(reinterpret_cast<unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&hash.m_data));
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
    
unsigned char* Scalar::data() {
    return reinterpret_cast<unsigned char*>(m_data);
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

Point Scalar::operator*(const Point& x) const {
    return (x * *this);
}

Point Scalar::operator*(const BasePoint& x) const {
    return (x * *this);
}

Scalar& Scalar::operator-() {
    Scalar result;

    crypto_core_ristretto255_scalar_negate(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&m_data));

    *this = result;

    return *this;
}

Scalar& Scalar::operator+=(const Scalar& x) {
    Scalar result;

    crypto_core_ristretto255_scalar_add(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&x.m_data));

    *this = result;

    return *this;
}    


    
Point::Point() {
}
    
Point::Point(const Scalar& scalar) {
    int e = crypto_scalarmult_ristretto255_base(reinterpret_cast<unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&scalar.m_data));
    if(e != 0)
        throw std::runtime_error("crypto_scalarmult_ristretto255_base failed");
}
    
Point::Point(const Hash<Point::HASHSIZE>& hash) {
    crypto_core_ristretto255_from_hash(reinterpret_cast<unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&hash.m_data));
}
    
Point Point::random() {
    Point result;
    Scalar exponent = Scalar::random();
    
    int e = crypto_scalarmult_ristretto255_base(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&exponent.m_data));
    if(e != 0)
        throw std::runtime_error("crypto_scalarmult_ristretto255_base failed");

    return result;
}

Point Point::from(const Scalar& x) {
    return Point(x);
}

const unsigned char* Point::data() const {
    return reinterpret_cast<const unsigned char*>(m_data);
}
    
unsigned char* Point::data() {
    return reinterpret_cast<unsigned char*>(m_data);
}

Point Point::operator+(const Point& x) const {
    Point result;

    int e = crypto_core_ristretto255_add(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&m_data), reinterpret_cast<const unsigned char*>(&x.m_data));
    if(e != 0)
        throw std::runtime_error("crypto_core_ristretto255_add failed");
    return result;
}

Point Point::operator*(const Scalar& x) const {
    Point result;

    int e = crypto_scalarmult_ristretto255(reinterpret_cast<unsigned char*>(&result.m_data), reinterpret_cast<const unsigned char*>(&x.m_data), reinterpret_cast<const unsigned char*>(&m_data));
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
    return (std::memcmp(x.m_data, y.m_data, crypto_core_ristretto255_SCALARBYTES) == 0);
}
    
bool operator!=(const Scalar& x, const Scalar& y) {
    return !(x == y);
}
    
bool operator==(const Point& x, const Point& y) {
    return (std::memcmp(x.m_data, y.m_data, crypto_core_ristretto255_BYTES) == 0);
}

bool operator!=(const Point& x, const Point& y) {
    return !(x == y);
}

BasePoint Commitment::G;
Point Commitment::H = hash<Point::HASHSIZE>(G);
    
Commitment::Commitment() {
}
    
Commitment::Commitment(const Scalar& value, const Scalar& blind)
{
    if(value == Scalar::zero()) {
        Point p = blind * H;
        static_cast<Point&>(*this) = p;
    } else {
        Point p = value * G + blind * H;
        static_cast<Point&>(*this) = p;
    }
}

}
}
}
