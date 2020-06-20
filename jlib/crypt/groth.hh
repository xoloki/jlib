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

#include <jlib/util/util.hh>

namespace jlib {
namespace crypt {
namespace groth {

class Point;
class Scalar;

template<int N>
class Hash {
public:
    static Hash generic(const Point& p);
    static Hash generic(const Scalar& x);
    static Hash generic(const unsigned char* bytes, std::size_t n);
 
    friend class Point; 
    friend class Scalar;
  
protected:
    unsigned char m_bytes[crypto_generichash_BYTES];
};

class Scalar {
public:
    Scalar operator+(const Scalar& x) const;
    Scalar operator*(const Scalar& x) const;
    Point operator*(const Point& x) const;
    Scalar& operator+=(const Scalar& x);
    Scalar& operator*=(const Scalar& x);

    const unsigned char* bytes() const;
    unsigned char* bytes();

    static Scalar random();

    template<int N>
    friend class Hash;
    friend class Point;

    friend std::ostream& operator<<(std::ostream& out, const Scalar& d);
    
protected:
    unsigned char m_bytes[crypto_core_ristretto255_SCALARBYTES];
};
    
class Point {
public:
    static const int HASHBYTES = crypto_core_ristretto255_HASHBYTES;

    Point();
    Point(const Scalar& scalar);
    Point(const Hash<HASHBYTES>& hash);

    Point operator+(const Point& x) const;
    Point operator*(const Scalar& x) const;
    Point& operator*=(const Scalar& x);
    
    const unsigned char* bytes() const;
    unsigned char* bytes();

    static Point random();
    static Point from(const Scalar& x);

    friend std::ostream& operator<<(std::ostream& out, const Point& d);

    template<int N>
    friend class Hash;
    
protected:
    unsigned char m_bytes[crypto_core_ristretto255_BYTES];
};

std::ostream& operator<<(std::ostream& out, const Scalar& d);

template<int N>
Hash<N> Hash<N>::generic(const Point& p) {
    return Hash<N>::generic(reinterpret_cast<const unsigned char*>(&p.m_bytes), crypto_core_ristretto255_BYTES);
}

template<int N>
Hash<N> Hash<N>::generic(const Scalar& x) {
    return Hash<N>::generic(reinterpret_cast<const unsigned char*>(&x.m_bytes), crypto_core_ristretto255_SCALARBYTES);
}
    
template<int N>
Hash<N> Hash<N>::generic(const unsigned char* bytes, std::size_t n) {
    Hash<N> result;
    
    crypto_generichash(reinterpret_cast<unsigned char*>(&result.m_bytes), crypto_generichash_BYTES, bytes, n, NULL, 0);

    return result;
}
    

    
}
}
}
