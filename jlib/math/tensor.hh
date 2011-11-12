/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joe Yandle <joey@divisionbyzero.com>
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

#ifndef JLIB_MATH_TENSOR_HH
#define JLIB_MATH_TENSOR_HH


#include <jlib/math/buffer.hh>
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
bool operator!=(const tensor<T>& a, const T& b);

template<typename T>
class tensor {
public:
    class mismatch : public std::exception {};
    class not_implemented : public std::exception {};
    
    template<typename U>
    friend tensor<U> operator*(const tensor<U>& a, const tensor<U>& b);

    template<typename U>
    friend tensor<U> operator^(const tensor<U>& a, const tensor<U>& b);

    explicit tensor(unsigned int r, ...);
    tensor(buffer<unsigned int> m);
    tensor(buffer<unsigned int> m, buffer<T> d);

    tensor<T> operator[](unsigned int i);

    // take ith slice.  in a rank 2 tensor, a slice is a column vector
    tensor<T> operator()(unsigned int i);

    operator T&();
    operator const T&() const;

    unsigned int rank() const;
    unsigned int size(unsigned int i) const;

    tensor<T>& operator=(const T& t);
    
private:
    unsigned int mmult(unsigned int o);

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
tensor<T> tensor<T>::operator()(unsigned int i) {
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


template<typename T>    
inline
tensor<T> operator^(const tensor<T>& a, const tensor<T>& b) {
    if(a.rank() == 2 && b.rank() == 2) {
        if(a.size(1) != b.size(0))
            throw typename tensor<T>::mismatch();

        // inner product of two matricies is a matrix with as many rows as the first and columns as the second
        tensor<T> ret(2, a.size(0), b.size(1));

        for(unsigned int j = 0; j < b.size(1); j++) {
            for(unsigned int i = 0; i < a.size(0); i++) {
                ret[i][j] = 0;
                for(unsigned int k = 0; k < a.size(1); k++) {
                    ret[i][j] += (a[i][k] * b[k][j]);
                }
            }
        }

        return ret;
    } else {
        throw typename tensor<T>::not_implemented();

        // inner product is a tensor with rank equal to sum of args' ranks - 2
        buffer<unsigned int> meta(a.rank() + b.rank() - 2);
        buffer<T> data();
        tensor<T> ret(meta, data);
    
        return ret;
    }
}


template<typename T>    
inline
tensor<T> operator*(const tensor<T>& a, const tensor<T>& b) {
    // outer product is a tensor with rank equal to sum of args' ranks
    buffer<unsigned int> meta(a.rank() + b.rank());
    buffer<T> data(a.meta.product() * b.meta.product());
    tensor<T> ret(meta, data);

    if(a.rank() == 1 && b.rank() == 1) {
        meta[0] = a.size(0);
        meta[1] = b.size(0);

        for(unsigned int i = 0; i < a.size(); i++) {
            for(unsigned int j = 0; j < b.size(); j++) {
                ret[i][j] = (a[i] * b[j]);
            }
        }

    } else {
        throw typename tensor<T>::not_implemented();
    }
    
    return ret;
}

template<typename T>    
bool operator==(const tensor<T>& a, const tensor<T>& b) {
    return (a.rank() == b.rank());
}

template<typename T>
bool operator!=(const tensor<T>& a, const T& b) {
    return !(a == b);
}
    

}
}

#endif //JLIB_MATH_TENSOR_HH
