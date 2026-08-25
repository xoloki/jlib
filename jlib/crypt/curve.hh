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

#ifndef JLIB_CRYPT_CURVE_HH
#define JLIB_CRYPT_CURVE_HH

#include <ostream>
#include <stdexcept>

#include <sodium.h>
#include <sodium/crypto_core_ristretto255.h>

#include <jlib/util/util.hh>

namespace jlib {
namespace crypt {
namespace curve {

class BasePoint;
class Point;
class Scalar;
class Commitment;

/**
 * NOTE: libsodium requires sodium_init() before any other call.  That is the
 * application's job, not this library's, so every program using jlib::crypt
 * must call it once at the top of main().
 *
 * This matters more than it looks: Commitment::G() and H() below used to be
 * namespace-scope objects, so they were constructed while the shared library
 * loaded, before main() could possibly have initialized libsodium.  On Linux
 * that meant crypto_generichash_update ran against an uninitialized library
 * and allocated until the process was OOM-killed, with no output at all.
 * They are function-local statics now, so a sodium_init() in main() is early
 * enough.
 */

template<int N>
class Hash {
public:
    Hash();

    void update(const Point& p);
    void update(const Scalar& x);
    void update(const Commitment& c);
    void update(const unsigned char* data, std::size_t n);
    void finalize();
    
    static Hash generic(const Point& p);
    static Hash generic(const Scalar& x);
    static Hash generic(const Commitment& x);
    static Hash generic(const unsigned char* data, std::size_t n);
 
    friend class Point; 
    friend class Scalar;
  
protected:
    // This was sized crypto_generichash_BYTES (32), but finalize() asks
    // libsodium for N bytes, and both instantiations use
    // crypto_core_ristretto255_HASHBYTES (64) -- so every finalize() wrote 32
    // bytes past the end of m_data and into m_state.
    static_assert(N >= crypto_generichash_BYTES_MIN && N <= crypto_generichash_BYTES_MAX,
                  "generichash output size out of range for libsodium");

    unsigned char m_data[N];
    crypto_generichash_state m_state;
};

template<int N, typename... Args>
Hash<N> hash(Args&&... args);

// These accumulate into hasher through the reference; they return nothing.
// They were declared to return Hash<N> while falling off the end without a
// return statement, which is undefined behaviour.  gcc 7.5 happened to emit a
// harmless fallthrough; gcc 13 treats the path as unreachable, which showed up
// as a SIGSEGV inside _Unwind_Resume with no exception in flight.
template<int N, typename T, typename... Args>
void do_hash(Hash<N>& hasher, const T& t, Args&&... args);

template<int N, typename T>
void do_hash(Hash<N>& hasher, const T& t);

class Scalar {
public:
    static const int HASHSIZE = crypto_core_ristretto255_HASHBYTES;
    static const int SIZE = crypto_core_ristretto255_SCALARBYTES;

    struct Power {
        static Scalar pow(const Scalar& x, const int& y) {
            return (x^y);
        }
    };
    
    Scalar();
    Scalar(std::size_t x);
    Scalar(const Hash<HASHSIZE>& hash);

    Scalar operator+(const Scalar& x) const;
    Scalar operator-(const Scalar& x) const;
    Scalar operator*(const Scalar& x) const;
    Scalar operator^(int k) const;
    Point operator*(const Point& x) const;
    Point operator*(const BasePoint& x) const;
    Scalar operator-() const;
    Scalar& operator+=(const Scalar& x);
    Scalar& operator-=(const Scalar& x);
    Scalar& operator*=(const Scalar& x);

    const unsigned char* data() const;
    unsigned char* data();

    static Scalar random();
    static Scalar zero();
    static Scalar one();
    
    template<int N>
    friend class Hash;
    friend class Point;
    friend class BasePoint;

    friend std::ostream& operator<<(std::ostream& out, const Scalar& d);
    friend bool operator==(const Scalar& x, const Scalar& y);
    
protected:
    unsigned char m_data[crypto_core_ristretto255_SCALARBYTES];
};
    
class Point {
public:
    static const int HASHSIZE = crypto_core_ristretto255_HASHBYTES;
    static const int SIZE = crypto_core_ristretto255_BYTES;

    Point();
    Point(const Scalar& scalar);
    Point(const Hash<HASHSIZE>& hash);

    Point operator+(const Point& x) const;
    Point operator-(const Point& x) const;
    Point operator*(const Scalar& x) const;
    Point& operator+=(const Point& x);
    Point& operator*=(const Scalar& x);
    
    const unsigned char* data() const;
    unsigned char* data();

    static Point zero();
    static Point random();
    static Point from(const Scalar& x);

    friend std::ostream& operator<<(std::ostream& out, const Point& d);
    friend bool operator==(const Point& x, const Point& y);

    template<int N>
    friend class Hash;
    friend class BasePoint;
    friend class Commitment;
    
protected:
    unsigned char m_data[crypto_core_ristretto255_BYTES];
};

class BasePoint : public Point {
public:
    BasePoint();

    Point operator*(const Scalar& x) const;
};

    
std::ostream& operator<<(std::ostream& out, const Scalar& d);
std::ostream& operator<<(std::ostream& out, const Point& d);

bool operator==(const Scalar& x, const Scalar& y);
bool operator!=(const Scalar& x, const Scalar& y);
bool operator==(const Point& x, const Point& y);
bool operator!=(const Point& x, const Point& y);

class Commitment : public Point {
public:
    Commitment();
    Commitment(const Scalar& value);
    Commitment(const Scalar& value, const Scalar& blind);
    
    const Scalar& value() const;
    const Scalar& blind() const;

    /**
     * The two generators, created on first use.
     *
     * These were namespace-scope objects, so constructing them -- which calls
     * into libsodium, and for H runs a hash -- happened while the shared
     * library was still loading, before main().  That is fragile by
     * construction: it ran before sodium_init() could have been called, and
     * the resulting failure was a bare OOM kill or segfault with no output.
     * Function-local statics defer the work to first use and are initialized
     * once, thread-safely.
     */
    static const BasePoint& G();
    static const Point& H();

protected:
    Scalar m_value;
    Scalar m_blind;
};
    
template<int N>
Hash<N>::Hash() {
    crypto_generichash_init(&m_state, 0, 0, N);
}

template<int N>
void Hash<N>::update(const Point& p) {
    crypto_generichash_update(&m_state, p.data(), Point::SIZE);
}
    
template<int N>
void Hash<N>::update(const Scalar& x) {
    crypto_generichash_update(&m_state, x.data(), Scalar::SIZE);
}
    
template<int N>
void Hash<N>::update(const Commitment& c) {
    crypto_generichash_update(&m_state, c.data(), Point::SIZE);
}
    
template<int N>
void Hash<N>::update(const unsigned char* data, std::size_t n) {
    crypto_generichash_update(&m_state, data, n);
}
    
template<int N>
void Hash<N>::finalize() {
    crypto_generichash_final(&m_state, reinterpret_cast<unsigned char*>(&m_data), N);
}
    
template<int N>
Hash<N> Hash<N>::generic(const Point& p) {
    return Hash<N>::generic(reinterpret_cast<const unsigned char*>(&p.m_data), Point::SIZE);
}

template<int N>
Hash<N> Hash<N>::generic(const Scalar& x) {
    return Hash<N>::generic(reinterpret_cast<const unsigned char*>(&x.m_data), Scalar::SIZE);
}

template<int N>
Hash<N> Hash<N>::generic(const Commitment& c) {
    return Hash<N>::generic(reinterpret_cast<const unsigned char*>(c.data()), Point::SIZE);
}

template<int N>
Hash<N> Hash<N>::generic(const unsigned char* data, std::size_t n) {
    Hash<N> result;
    
    crypto_generichash(reinterpret_cast<unsigned char*>(&result.m_data), crypto_generichash_BYTES, data, n, NULL, 0);

    return result;
}
    
template<int N, typename... Args>
Hash<N> hash(Args&&... args) {
    Hash<N> hasher;

    do_hash(hasher, args...);
    hasher.finalize();
    
    return hasher;
}

template<int N, typename T, typename... Args>
void do_hash(Hash<N>& hasher, const T& t, Args&&... args) {
    hasher.update(t.data(), T::SIZE);
    do_hash(hasher, args...);
}

template<int N, typename T>
void do_hash(Hash<N>& hasher, const T& t) {
    hasher.update(t.data(), T::SIZE);
}

}
}
}

#endif
