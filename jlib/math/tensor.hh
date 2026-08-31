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

#ifndef JLIB_MATH_TENSOR_HH
#define JLIB_MATH_TENSOR_HH


#include <jlib/math/buffer.hh>
#include <array>
#include <cstdarg>


namespace jlib {
namespace math {


template<typename T>
class tensor;

// tensor product
template<typename T> 
tensor<T> operator*(const tensor<T>& a, const tensor<T>& b);

// tensor inner (dot) product
template<typename T>    
tensor<T> operator^(const tensor<T>& a, const tensor<T>& b);

template<typename T>    
bool operator==(const tensor<T>& a, const tensor<T>& b);

template<typename T>    
bool operator!=(const tensor<T>& a, const tensor<T>& b);

template<typename T>
class tensor {
public:
    class mismatch : public std::exception {};
    class not_implemented : public std::exception {};
    
    template<typename U>
    friend tensor<U> operator*(const tensor<U>& a, const tensor<U>& b);

    template<typename U>
    friend tensor<U> operator^(const tensor<U>& a, const tensor<U>& b);

    template<typename U>
    friend bool operator==(const tensor<U>& a, const tensor<U>& b);

    explicit tensor(unsigned int r, ...);
    tensor(buffer<unsigned int> m);
    tensor(buffer<unsigned int> m, buffer<T> d);

    /**
     * Slice the first axis: a view, sharing storage with this tensor.
     *
     * The const overload returns a const tensor, so `t[i][j] = x` on a const
     * tensor does not compile.  The constness is shallow, as it is for any
     * handle type -- copying the result into a non-const tensor shares the same
     * storage and can write through it.  Blocking that needs a distinct const
     * view type; see #143.
     */
    tensor<T> operator[](unsigned int i);
    const tensor<T> operator[](unsigned int i) const;

    /**
     * Element access by a full index: t(i, j, k) on a rank 3 tensor.
     *
     * The same meaning operator() has on matrix, which is m(r, c) -- and no
     * longer the meaning it had here, where it was a byte-for-byte copy of
     * operator[] under a comment claiming it returned a column.  Nothing called
     * it, so nothing changes behaviour; what changes is that the two classes in
     * this directory now agree about what the operator is for.
     *
     * Throws mismatch if the number of indices is not the rank.  Individual
     * indices are not bounds-checked, matching matrix, which keeps the branch
     * out of element access.
     */
    template<typename... Args>
    T& operator()(Args... args);

    template<typename... Args>
    const T& operator()(Args... args) const;

    operator T&();
    operator const T&() const;

    unsigned int rank() const;
    unsigned int size(unsigned int i) const;

    tensor<T>& operator=(const T& t);
    
private:
    unsigned int mmult(unsigned int o);

    template<typename... Args>
    unsigned int flat(Args... args) const;

    buffer<unsigned int> meta;
    buffer<T> data;
};

    
template<typename T>
inline
tensor<T>::tensor(unsigned int r, ...)
    : meta(r)
{
    va_list v;
    va_start(v, r);

    unsigned int size = 1;

    for(unsigned int i = 0; i < meta.size(); i++) {
        unsigned int val = va_arg(v, unsigned int);
        meta[i] = val;
        size *= val;
    }

    va_end(v);

    data.resize(size);
}

template<typename T>
inline
tensor<T>::tensor(buffer<unsigned int> m) 
    : meta(m)
{
    data.resize(meta.product());
}

template<typename T>
inline
tensor<T>::tensor(buffer<unsigned int> m, buffer<T> d) 
    : meta(m),
      data(d)
{
}

template<typename T>
inline
tensor<T>::operator T&() {
    if(meta.size())
        throw mismatch();

    return data[0];
}

template<typename T>
inline
tensor<T>::operator const T&() const {
    if(meta.size())
        throw mismatch();

    return data[0];
}


template<typename T>
inline
tensor<T> tensor<T>::operator[](unsigned int i) {
    if(!meta.size()) {
        if(i)
            throw mismatch();
        else
            return *this;
    }

    buffer<unsigned int> submeta(meta, 1, meta.size() - 1);

    unsigned int p = submeta.product();

    buffer<T> subdata(data, i * p, p);

    return tensor<T>(submeta, subdata);
}

template<typename T>
inline
const tensor<T> tensor<T>::operator[](unsigned int i) const {
    if(!meta.size()) {
        if(i)
            throw mismatch();
        else
            return *this;
    }

    buffer<unsigned int> submeta(meta, 1, meta.size() - 1);

    unsigned int p = submeta.product();

    buffer<T> subdata(data, i * p, p);

    return tensor<T>(submeta, subdata);
}

template<typename T>
template<typename... Args>
inline
unsigned int tensor<T>::flat(Args... args) const {
    // std::array rather than a C array because sizeof...(Args) is 0 for a rank
    // 0 tensor, and a zero-length C array is not a thing.  t() is how you reach
    // the single element of a scalar.
    const std::array<unsigned int, sizeof...(Args)> idx =
        { static_cast<unsigned int>(args)... };

    if(sizeof...(Args) != rank())
        throw mismatch();

    // Row-major, so the last index is the fastest and each axis multiplies in
    // as it is passed: ((i0 * d1 + i1) * d2 + i2) ...  Same layout operator[]
    // slices with, which is what makes the two agree.
    unsigned int f = 0;

    for(unsigned int i = 0; i < sizeof...(Args); i++)
        f = f * meta[i] + idx[i];

    return f;
}

template<typename T>
template<typename... Args>
inline
T& tensor<T>::operator()(Args... args) {
    return data[flat(args...)];
}

template<typename T>
template<typename... Args>
inline
const T& tensor<T>::operator()(Args... args) const {
    return data[flat(args...)];
}

template<typename T>
inline
unsigned int tensor<T>::rank() const {
    return meta.size();
}

template<typename T>
inline
unsigned int tensor<T>::size(unsigned int i) const {
    return meta[i];
}

template<typename T>
inline
unsigned int tensor<T>::mmult(unsigned int o) {
    unsigned int m = 1;

    for(unsigned int i = o; i < meta.size(); i++) {
        m *= meta[i];
    }

    return m;
}

template<typename T>
inline
tensor<T>& tensor<T>::operator=(const T& t) {
    if(rank())
        throw mismatch();

    data[0] = t;

    return *this;
}


/**
 * Inner product: contract a's last axis against b's first.
 *
 * The result has rank a.rank() + b.rank() - 2, and its shape is a's axes
 * without the last followed by b's without the first.  Contracting the last
 * against the first is the pairing that makes rank 2 ^ rank 2 the matrix
 * product, which is why the rank-2 special case this replaces had already
 * chosen it; the general case here is that one generalised, not a second
 * convention beside it.  Rank 1 ^ rank 1 is therefore rank 0 -- the dot
 * product, delivered as a scalar.
 */
template<typename T>    
inline
tensor<T> operator^(const tensor<T>& a, const tensor<T>& b) {
    // Both operands need an axis to contract.  Without this the rank
    // arithmetic below underflows -- a.rank() + b.rank() - 2 is unsigned -- and
    // asks for a shape of four billion axes.
    if(a.rank() < 1 || b.rank() < 1)
        throw typename tensor<T>::mismatch();

    const unsigned int k = a.meta[a.rank() - 1];

    if(k != b.meta[0])
        throw typename tensor<T>::mismatch();

    buffer<unsigned int> meta(a.rank() + b.rank() - 2);

    // m and n are multiplied out here rather than divided out of the totals,
    // which costs nothing and means an axis of length zero yields an empty
    // result instead of dividing by k.
    unsigned int m = 1;
    unsigned int n = 1;

    for(unsigned int i = 0; i + 1 < a.rank(); i++) {
        meta[i] = a.meta[i];
        m *= a.meta[i];
    }

    for(unsigned int i = 1; i < b.rank(); i++) {
        meta[a.rank() + i - 2] = b.meta[i];
        n *= b.meta[i];
    }

    tensor<T> ret(meta);

    // The storage is row-major -- operator[] slices the first axis at a stride
    // of the remaining axes' product -- so a is *already* laid out as an m x k
    // matrix and b as a k x n one.  The axes being contracted are adjacent in
    // memory by construction, so general contraction is matrix multiply over
    // the flattened views: no gather, no strides to carry, and no special case
    // for rank 2, which now falls out of this rather than sitting beside it.
    for(unsigned int i = 0; i < m; i++) {
        for(unsigned int j = 0; j < n; j++) {
            T sum = T();

            for(unsigned int p = 0; p < k; p++)
                sum += (a.data[i * k + p] * b.data[p * n + j]);

            ret.data[i * n + j] = sum;
        }
    }

    return ret;
}


/**
 * Outer product: every element of a against every element of b.
 *
 * The result has rank a.rank() + b.rank() and its shape is the two shapes
 * concatenated.  Rank 0 is not an error but the scaling case -- concatenating
 * an empty shape gives back the other one, so a scalar times a tensor is that
 * tensor scaled.
 */
template<typename T>    
inline
tensor<T> operator*(const tensor<T>& a, const tensor<T>& b) {
    buffer<unsigned int> meta(a.rank() + b.rank());

    for(unsigned int i = 0; i < a.rank(); i++)
        meta[i] = a.meta[i];

    for(unsigned int i = 0; i < b.rank(); i++)
        meta[a.rank() + i] = b.meta[i];

    // Filled before the tensor is built, not after.  The version this replaces
    // constructed ret from an unfilled meta and wrote the shape in afterwards,
    // which reached ret at all only because buffer is a shared handle rather
    // than a value -- right by accident of the ownership model.
    tensor<T> ret(meta);

    const unsigned int m = a.meta.product();
    const unsigned int n = b.meta.product();

    // Row-major again: concatenating the shapes concatenates the layouts, so
    // result element (i,j) is at flat i*n + j whatever the two ranks are.
    for(unsigned int i = 0; i < m; i++)
        for(unsigned int j = 0; j < n; j++)
            ret.data[i * n + j] = (a.data[i] * b.data[j]);

    return ret;
}

/**
 * Equal shapes and equal elements.
 *
 * Was `a.rank() == b.rank()`, which made every matrix equal to every other
 * matrix.  Nothing called it and nothing could have -- operator!= below passed
 * it a scalar, so neither would compile -- but a test of the products has to
 * compare two of them, and it cannot be built on that.
 */
template<typename T>    
bool operator==(const tensor<T>& a, const tensor<T>& b) {
    if(a.rank() != b.rank())
        return false;

    for(unsigned int i = 0; i < a.rank(); i++)
        if(a.size(i) != b.size(i))
            return false;

    const unsigned int n = a.meta.product();

    for(unsigned int i = 0; i < n; i++)
        if(!(a.data[i] == b.data[i]))
            return false;

    return true;
}

template<typename T>
bool operator!=(const tensor<T>& a, const tensor<T>& b) {
    return !(a == b);
}

    

}
}

#endif //JLIB_MATH_TENSOR_HH
