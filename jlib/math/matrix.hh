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

#ifndef JLIB_MATH_MATRIX_HH
#define JLIB_MATH_MATRIX_HH

#include <jlib/math/buffer.hh>

#include <iostream>
#include <iomanip>
#include <exception>
#include <vector>
#include <map>
#include <list>
#include <string>
#include <sstream>
#include <functional>

#include <cmath>
#include <cstdarg>

typedef unsigned int uint;

namespace jlib {
namespace math {

template<typename T>
class vertex;

struct plane {
    uint i;
    uint j;
};

template<typename T>
class matrix {
public:
    enum factorization { LU, LDU };
    enum PLANE { XY, XZ, XW, YZ, YW, ZW };

    class mismatch : public std::exception {};
    class singular : public std::exception {};
    class mismatched : public std::runtime_error {
    public:
	mismatched(uint ar, uint ac, uint br, uint bc)
	    : std::runtime_error(format(ar, ac, br, bc))
	{}

	static std::string format(uint ar, uint ac, uint br, uint bc) {
	    std::ostringstream o;
	    o << "mismatch matricies [" << ar << "," << ac << "], [" << br << "," << bc << "]";
	    return o.str();
	}
    };

    matrix(uint rows, uint cols);
    matrix(uint rows, uint cols, const matrix<T>& m, uint roff, uint coff);

    T& operator()(uint r, uint c);
    const T& operator()(uint r, uint c) const;

    matrix<T>& operator*=(const matrix<T>& m);
    matrix<T>& operator+=(const matrix<T>& m);
    matrix<T>& operator-=(const matrix<T>& m);
    
    operator buffer<T>();
    operator const buffer<T>() const;

    matrix<T> transpose() const;

    void foreach(std::function<void (T&)> handler);
    void foreach_index(std::function<void (uint r, uint c, T&)> handler);
    
    //matrix<T> row(uint i) const;
    //matrix<T> col(uint i) const;

    std::map<std::string, matrix<T> > factor(factorization f) const;
    
    static matrix<T> identity(uint n);
    static matrix<T> diagonal(uint n, T x);
    static matrix<T> diagonalv(uint n, ...);
    
    static matrix<T> project(uint n, std::vector< std::pair<T,T> > clip);
    static matrix<T> translate(uint n, const vertex<T>& v);
    static matrix<T> rotate(uint n, plane p, double rad);
    static matrix<T> rotate(uint n, PLANE p, double rad);
    static matrix<T> lookAt(uint n, const vertex<T>& eye, 
                            const vertex<T>& up, const vertex<T>& center);

    uint M;
    uint N;

protected:
    static T projectd(T x1, T x2, T z1);
    static T projectc(T x1, T x2);
    static T projectdz(T x1, T x2);
    static T projectcz(T x1, T x2);

    //std::vector< std::vector<T> > rep;
    // use buffer in column-major mode so we can pass data off to GL without copying
    // also, this lets us represent a column vector simply
    buffer<T> rep;

    bool transposed = false;
};


template<typename T>    
matrix<T> operator*(const matrix<T>& a, const matrix<T>& b);

template<typename T>    
matrix<T> operator^(const matrix<T>& a, const matrix<T>& b);

template<typename T>    
matrix<T> strassen(const matrix<T>& a, const matrix<T>& b);

template<typename T>    
matrix<T> operator*(const matrix<T>& a, const T& b);

template<typename T>    
matrix<T> operator*(const T& b, const matrix<T>& a);

template<typename T>    
matrix<T> operator+(const matrix<T>& a, const T& b);

template<typename T>    
matrix<T> operator+(const T& b, const matrix<T>& a);

template<typename T>    
matrix<T> operator-(const matrix<T>& a, const T& b);

template<typename T>    
matrix<T> operator-(const T& b, const matrix<T>& a);

template<typename T>    
matrix<T> operator+(const matrix<T>& a, const matrix<T>& b);

template<typename T>    
matrix<T> operator-(const matrix<T>& a, const matrix<T>& b);

template<typename T>    
bool operator==(const matrix<T>& a, const matrix<T>& b);

template<typename T>    
bool operator!=(const matrix<T>& a, const matrix<T>& b);

template<typename T>    
std::istream& operator>>(std::istream& s, matrix<T>& m);

template<typename T>    
std::ostream& operator<<(std::ostream& s, const matrix<T>& m);


template<typename T>
class vertex {
public:
    vertex(uint n);
    vertex(const std::vector<T>& v);

    T& operator[](uint i);
    const T& operator[](uint i) const;
    matrix<T>& operator()();
    const matrix<T>& operator()() const;
    operator matrix<T>();
    vertex<T>& operator=(const matrix<T>& m);
    vertex<T>& operator=(const vertex<T>& v);

    bool operator<(const vertex<T>& v) const;
    
    void normalize();
    void change(uint n);

    T* data();
    const T* data() const;

    uint D;

protected:
    matrix<T> col;
};


template<typename T>    
bool operator==(const vertex<T>& a, const vertex<T>& b);

template<typename T>    
bool operator!=(const vertex<T>& a, const vertex<T>& b);


template<typename T>
class object {
public:
    object(uint n);

    void normalize();
    
    vertex<T>& operator[](uint x);
    const vertex<T>& operator[](uint x) const;
    uint size() const;

    std::list< vertex<T> > adjacent(uint x);

    void change(uint n);

    uint D;

protected:
    std::vector<vertex<T> > v;
    std::vector< std::list< vertex<T> > > adj;
};


template<typename T>
class cuboid : public object<T> {
public:
    cuboid(uint n);
};


template<typename T>
class pyramoid : public object<T> {
public:
    pyramoid(uint n);
};


template<typename T>
class staroid : public object<T> {
public:
    staroid(uint n);
};

template<typename T>
class spheroid : public object<T> {
public:
    spheroid(uint n);
};


template<typename T>    
matrix<T> operator*(const matrix<T>& a, const object<T>& b);


template<typename T>    
inline
matrix<T> operator*(const matrix<T>& a, const matrix<T>& b) {
    if(a.N != b.M)
        throw typename matrix<T>::mismatched(a.M, a.N, b.M, b.N);
    
    matrix<T> ret(a.M, b.N);
    
    for(uint j = 0; j < b.N; j++) {
        for(uint i = 0; i < a.M; i++) {
            //ret(i,j) = matrix<T>::dot(a.row(i), col);
            T sum = 0;
            for(uint k = 0; k < a.N; k++) {
                sum += a(i,k) * b(k,j); 
            }
            ret(i,j) = sum;
        }
    }

    return ret;
}

template<typename T>    
inline
matrix<T> operator^(const matrix<T>& a, const matrix<T>& b) {
    if(a.M != b.M || a.N != b.N)
        throw typename matrix<T>::mismatched(a.M, a.N, b.M, b.N);
    
    matrix<T> ret(a.M, b.N);
    
    for(uint j = 0; j < b.N; j++) {
        for(uint i = 0; i < a.M; i++) {
            ret(i,j) = a(i, j) * b(i, j);
        }
    }

    return ret;
}

template<typename T>    
inline
matrix<T> strassen(const matrix<T>& a, const matrix<T>& b) {
    
}


template<typename T>    
inline
matrix<T> operator*(const matrix<T>& a, const T& b) {
    matrix<T> ret(a.M, a.N);
    for(uint i = 0; i < a.M; i++) {
        for(uint j = 0; j < a.N; j++) {
            ret(i,j) = a(i, j) * b;
        }
    }
    return ret;
}

template<typename T>    
inline
matrix<T> operator*(const T& b, const matrix<T>& a) {
    matrix<T> ret(a.M, a.N);
    for(uint i = 0; i < a.M; i++) {
        for(uint j = 0; j < a.N; j++) {
            ret(i,j) = a(i, j) * b;
        }
    }
    return ret;
}


template<typename T>    
inline
matrix<T> operator+(const matrix<T>& a, const T& b) {
    matrix<T> ret(a.M, a.N);
    for(uint i = 0; i < a.M; i++) {
        for(uint j = 0; j < a.N; j++) {
            ret(i,j) = a(i, j) + b;
        }
    }
    return ret;
}

template<typename T>    
inline
matrix<T> operator+(const T& b, const matrix<T>& a) {
    matrix<T> ret(a.M, a.N);
    for(uint i = 0; i < a.M; i++) {
        for(uint j = 0; j < a.N; j++) {
            ret(i,j) = a(i, j) + b;
        }
    }
    return ret;
}


template<typename T>    
inline
matrix<T> operator-(const matrix<T>& a, const T& b) {
    matrix<T> ret(a.M, a.N);
    for(uint i = 0; i < a.M; i++) {
        for(uint j = 0; j < a.N; j++) {
            ret(i,j) = a(i, j) - b;
        }
    }
    return ret;
}

template<typename T>    
inline
matrix<T> operator-(const T& b, const matrix<T>& a) {
    matrix<T> ret(a.M, a.N);
    for(uint i = 0; i < a.M; i++) {
        for(uint j = 0; j < a.N; j++) {
            ret(i,j) = b - a(i, j);
        }
    }
    return ret;
}


template<typename T>    
inline
matrix<T> operator+(const matrix<T>& a, const matrix<T>& b) {
    if(a.M != b.M || a.N != b.N)
        throw matrix<T>::mismatch();

    matrix<T> ret(a.M, a.N);
    for(uint i = 0; i < a.M; i++) {
        for(uint j = 0; j < a.N; j++) {
            ret(i,j) = a(i, j) + b(i, j);
        }
    }
    return ret;    
}


template<typename T>    
inline
matrix<T> operator-(const matrix<T>& a, const matrix<T>& b) {
    if(a.M != b.M || a.N != b.N)
        throw typename matrix<T>::mismatch();

    matrix<T> ret = a;
    for(uint i = 0; i < a.M; i++) {
        for(uint j = 0; j < a.N; j++) {
	    ret(i,j) = a(i, j) - b(i, j);
        }
    }
    return ret;    
}




template<typename T>    
inline
bool operator!=(const matrix<T>& a, const matrix<T>& b) {
    return !(a == b);
}

template<typename T>    
inline
bool operator==(const matrix<T>& a, const matrix<T>& b) {
    if(a.M != b.M || a.N != b.N)
        throw typename matrix<T>::mismatch();
    
    for(uint i = 0; i < a.M; i++) {
        for(uint j = 0; j < b.N; j++) {
            if (a(i,j) != b(i,j))
                return false;
        }
    }

    return true;
}


template<typename T>    
inline
bool operator!=(const vertex<T>& a, const vertex<T>& b) {
    return !(a == b);
}

template<int D,typename T>    
inline
bool operator==(const vertex<T>& a, const vertex<T>& b) {
    return a() == b();
}


template<typename T>    
inline
std::istream& operator>>(std::istream& s, matrix<T>& m) {
    for(uint i = 0; i < m.M; i++) {
        for(uint j = 0; j < m.N; j++) {
            s >> m(i,j);
        }
    }

    return s;
}


template<typename T>    
inline
std::ostream& operator<<(std::ostream& s, const matrix<T>& m) {
    std::vector<int> widths(m.N);

    for(uint j = 0; j < m.N; j++) {
        for(uint i = 0; i < m.M; i++) {
            std::ostringstream o;
            o << m(i, j);
            if(o.str().length() > widths[j]) {
                widths[j] = o.str().length();
            }
        }
    }
    
    for(uint i = 0; i < m.M; i++) {
        //s << "[";
        for(uint j = 0; j < m.N; j++) {
            s << std::setw(widths[j]) << m(i,j);
            if(j + 1 < m.N) {
                s << " ";
            }
        }
        //s << "]";
        
        if(i + 1 < m.M) {
            s << "\n";
        }
    }	    
    return s;
}


template<typename T>
inline
matrix<T>::matrix(uint rows, uint cols) 
    : M(rows),
      N(cols),
      rep(rows*cols)
{
}

template<typename T>
inline
matrix<T>::matrix(uint rows, uint cols, const matrix<T>& m, uint roff, uint coff)
{

}


template<typename T>
inline
void matrix<T>::foreach(std::function<void (T&)> handler) {
    for(uint i = 0; i < this->M; i++) {
        for(uint j = 0; j < this->N; j++) {
            handler((*this)(i,j));
        }
    }
}
    
template<typename T>
inline
void matrix<T>::foreach_index(std::function<void (uint,uint,T&)> handler) {
    for(uint i = 0; i < this->M; i++) {
        for(uint j = 0; j < this->N; j++) {
            handler(i, j, (*this)(i,j));
        }
    }
}
    
template<typename T>
inline
T& matrix<T>::operator()(uint r, uint c) {
    if(!transposed)
	return rep[c * M + r];
    else
	return rep[r * N + c];
}


template<typename T>
inline
const T& matrix<T>::operator()(uint r, uint c) const {
    if(!transposed)
	return rep[c * M + r];
    else
	return rep[r * N + c];
}


template<typename T>
inline
matrix<T>& matrix<T>::operator*=(const matrix<T>& m) {
    *this = (*this) * m;
    return *this;
}

template<typename T>    
inline
matrix<T>& matrix<T>::operator+=(const matrix<T>& b) {
    if(this->M != b.M || this->N != b.N)
        throw typename matrix<T>::mismatch();

    matrix<T>& ret = *this;
    for(uint i = 0; i < this->M; i++) {
        for(uint j = 0; j < this->N; j++) {
            ret(i,j) += b(i, j);
        }
    }
    
    return ret;    
}


template<typename T>    
inline
matrix<T>& matrix<T>::operator-=(const matrix<T>& b) {
    if(this->M != b.M || this->N != b.N)
        throw typename matrix<T>::mismatch();

    matrix<T>& ret = *this;
    for(uint i = 0; i < this->M; i++) {
        for(uint j = 0; j < this->N; j++) {
	    ret(i,j) -= b(i, j);
        }
    }
    return ret;    
}

template<typename T>
inline
matrix<T>::operator buffer<T>() {
    return rep;
}


template<typename T>
inline
matrix<T>::operator const buffer<T>() const {
    return rep;
}

template<typename T>
inline
matrix<T> matrix<T>::transpose() const {
    matrix<T> ret(N, M);

    ret.rep = rep;
    ret.transposed = !transposed;

    return ret;
}

/*
template<typename T>
inline
std::vector<T> matrix<T>::row(uint x) const {
    return rep[x];
}


template<typename T>
inline
std::vector<T> matrix<T>::col(uint x) const {
    std::vector<T> ret;
    for(uint i = 0; i < M; i++) {
        ret.push_back(rep[i][x]);
    }
    return ret;
}

template<typename T>
inline
T matrix<T>::dot(const std::vector<T>& a, const std::vector<T>& b) {
    if(a.size() != b.size())
        throw typename matrix<T>::mismatch();
    
    T sum = 0;
    for(uint i = 0; i < a.size(); i++) {
        sum += (a[i] * b[i]);
    }
    
    return sum;
}
    */


template<typename T>
inline
matrix<T> matrix<T>::identity(uint n) {
    matrix<T> ret(n, n);
    
    for(uint i = 0; i < n; i++) {
        ret(i,i) = 1;
    }
    
    return ret;
}
    
    
template<typename T>
inline
matrix<T> matrix<T>::diagonal(uint n, T x) {
    matrix<T> ret(n, n);
    
    for(uint i = 0; i < n; i++) {
        ret(i,i) = x;
    }    
    
    return ret;
}


template<typename T>
inline
matrix<T> matrix<T>::diagonalv(uint n, ...) {
    matrix<T> ret(n, n);
    va_list v;
    
    va_start(v, n);
    for(uint i = 0; i < n; i++) {
        T t = va_arg(v, T);
        ret(i,i) = t;
    }
    va_end(v);
    
    return ret;
}
    

template<typename T>
inline
std::map<std::string, matrix<T> > matrix<T>::factor(factorization f) const {
    std::map<std::string, matrix<T> > ret;
    
    matrix<T> L(M, N);
    matrix<T> D(M, N);
    matrix<T> U = *this;
    matrix<T> P = identity(M);
    
    for(uint j = 0; j < N; j++) {
        /*
        std::vector<T> column = col(j);
        uint i = 0;
        while(i < column.size() && column[i] == 0) i++;
        
        if(i == column.size()) // no non-zero pivot found for this col
            throw typename matrix<T>::singular();
        
        if(i != 0) { // permutation necessary
            P[i][i] = 0;
            P[i][j] = 1;
            P[j][i] = 1;
            P[j][j] = 0;
        }
        */
        for(uint i = 0; i < M; i++) {
        }
    }
    
    switch(f) {
    case LU:
        ret.insert(ret.begin(), std::make_pair("L", L));
        ret.insert(ret.begin(), std::make_pair("U", U));
        ret.insert(ret.begin(), std::make_pair("P", P));
        break;
    case LDU:
        ret.insert(ret.begin(), std::make_pair("L", L));
        ret.insert(ret.begin(), std::make_pair("D", D));
        ret.insert(ret.begin(), std::make_pair("U", U));
        ret.insert(ret.begin(), std::make_pair("P", P));
        break;
    }
    
    return ret;
}


template<typename T>
inline
matrix<T> matrix<T>::project(uint n, std::vector< std::pair<T,T> > clip) {
    uint z = (n - 1);
    uint w = (n);
    matrix<T> ret(n+1, n+1);
    
    ret(w,z) = -1;
    
    for(uint i = 0; i < z; i++) {
        ret(i,i) = projectd(clip[i].first, clip[i].second, clip[z].first);
        ret(i,z) = projectc(clip[i].first, clip[i].second);
    }
    
    ret(z,z) = projectdz(clip[z].first, clip[z].second);
    ret(z,w) = projectcz(clip[z].first, clip[z].second);

    return ret;
}

template<typename T>
inline
matrix<T> matrix<T>::translate(uint n, const vertex<T>& v) {
    matrix<T> ret = identity(n+1);
    
    for(uint i = 0; i < n; i++) {
        ret(i,n) = v[i];
    }

    return ret;
}


template<typename T>
inline
matrix<T> matrix<T>::rotate(uint n, plane p, double rad) {
    matrix<T> ret = identity(n+1);
    double sin = std::sin(rad);
    double cos = std::cos(rad);

    ret(p.i,p.i) = cos;
    if((p.i == 0 && p.j == 1) || (p.i == 1 && p.j == 2) || 
       (p.i == 0 && p.j == 3)) {
        ret(p.i,p.j) = sin;
        ret(p.j,p.i) = -sin;
    } else {
        ret(p.i,p.j) = -sin;
        ret(p.j,p.i) = sin;        
    }
    ret(p.j,p.j) = cos;

    return ret;
}


template<typename T>
inline
matrix<T> matrix<T>::rotate(uint n, PLANE p, double rad) {
    matrix<T> ret = identity(n+1);
    double sin = std::sin(rad);
    double cos = std::cos(rad);

    switch(p) {
    case XY:
        ret(0,0) = cos;
        ret(0,1) = sin;
        ret(1,0) = -sin;
        ret(1,1) = cos;
        break;
    case YZ:
        ret(1,1) = cos;
        ret(1,2) = sin;
        ret(2,1) = -sin;
        ret(2,2) = cos;
        break;
    case XZ:
        ret(0,0) = cos;
        ret(0,2) = -sin;
        ret(2,0) = sin;
        ret(2,2) = cos;
        break;
    case XW:
        ret(0,0) = cos;
        ret(0,3) = sin;
        ret(3,0) = -sin;
        ret(3,3) = cos;
        break;
    case YW:
        ret(1,1) = cos;
        ret(1,3) = -sin;
        ret(3,1) = sin;
        ret(3,3) = cos;
        break;
    case ZW:
        ret(2,2) = cos;
        ret(2,3) = -sin;
        ret(3,2) = sin;
        ret(3,3) = cos;
        break;
    default:
        throw mismatch();
    }

    return ret;
}


template<typename T>
inline
matrix<T> matrix<T>::lookAt(uint n, const vertex<T>& eye, 
                            const vertex<T>& up, const vertex<T>& center) {
    matrix<T> ret = matrix<T>::identity(n+1);
    const T neg = static_cast<T>(-1);
    vertex<T> neye(n); neye = (eye() * neg);
    
    ret *= translate(n, neye);

    /*
    for(uint i = 0; i <n; i++) {
        plane p;
        p.i = i;
        p.j = 1;
    }
    */

    return ret;
}


template<typename T>
inline
T matrix<T>::projectd(T x1, T x2, T z1) {
    return (2 * z1 / (x2 - x1));
}


template<typename T>
inline
T matrix<T>::projectc(T x1, T x2) {
    return (x2 + x1) / (x2 - x1);
}


template<typename T>
inline
T matrix<T>::projectdz(T x1, T x2) {
    return (-1) * projectc(x1, x2);
}


template<typename T>
inline
T matrix<T>::projectcz(T x1, T x2) {
    return (-2 * x2 * x1 / (x2 - x1));
}

    //vertex<T>::

template<typename T>
inline
vertex<T>::vertex(uint n) 
    : D(n),
      col(n+1,1)
{
    col(n,0) = 1;
}


template<typename T>
inline
vertex<T>::vertex(const std::vector<T>& v)
    : D(v.size()),
      col(v.size()+1,1)
{
    for(uint i = 0; i < v.size(); i++) {
        col(i,0) = v[i];
    }
    col(D,0) = 1;
}


template<typename T>
inline
T& vertex<T>::operator[](uint i) {
    return col(i,0);
}


template<typename T>
inline
const T& vertex<T>::operator[](uint i) const {
    return col(i,0);
}


template<typename T>
inline
matrix<T>& vertex<T>::operator()() {
    return col;
}


template<typename T>
inline
const matrix<T>& vertex<T>::operator()() const {
    return col;
}


template<typename T>
inline
vertex<T>::operator matrix<T>() {
    return col;
}


template<typename T>
inline
vertex<T>& vertex<T>::operator=(const matrix<T>& m) {
    if(m.M > col.M) 
        throw typename matrix<T>::mismatch();

    for(uint i = 0; i < m.M; i++) {
        (*this)[i] = m(i,0);
    }

    return *this;
}


template<typename T>
inline
vertex<T>& vertex<T>::operator=(const vertex<T>& v) {
    for(uint i = 0; i < std::min(D, v.D); i++) {
        (*this)[i] = v[i];
    }

    return *this;
}


template<typename T>
inline
bool vertex<T>::operator<(const vertex<T>& v) const {
    //return data() < v.data();
    
    for(uint i = 0; i < std::min(D, v.D); i++) {
        const T& ti = (*this)[i];
        const T& vi = v[i];
        if(ti != vi) {
            return ti < vi;
        }
    }

    if(D != v.D) {
        return D < v.D;
    }

    return false;

}


template<typename T>
inline
void vertex<T>::normalize() {
    for(uint i = 0; i < (D+1); i++) {
        (*this)[i] /= (*this)[D];
    }
}


template<typename T>
inline
void vertex<T>::change(uint n) {
    vertex<T> ncol(n);
    ncol = *this;
    col = ncol;
    D = n;
}


template<typename T>
inline
T* vertex<T>::data() {
    return static_cast< buffer<T> >(col).data();
}


template<typename T>
inline
const T* vertex<T>::data() const {
    return static_cast< buffer<T> >(col).data();
}


template<typename T>
inline
object<T>::object(uint n) 
    : D(n)
{
}


template<typename T>
inline
void object<T>::normalize() {
    for(uint i = 0; i < v.size(); i++) {
        v[i].normalize();
    }
}
    
template<typename T>
inline
vertex<T>& object<T>::operator[](uint x) {
    return v[x];
}

template<typename T>
inline
const vertex<T>& object<T>::operator[](uint x) const {
    return v[x];
}


template<typename T>
inline
uint object<T>::size() const {
    return v.size();
}


template<typename T>
inline
std::list< vertex<T> > object<T>::adjacent(uint x) {
    return adj[x];
}

template<typename T>
inline
void object<T>::change(uint n) {
    for(uint i = 0; i < size(); i++) {
        v[i].change(n);

        std::list< vertex<T> >& adjacent = adj[i];
        typename std::list< vertex<T> >::iterator j = adjacent.begin();
        for(; j != adjacent.end(); j++) {
            j->change(n);
        }
    }
}


template<typename T>
inline
cuboid<T>::cuboid(uint n) 
    : object<T>(n)
{
    std::vector<T> vals(this->D), adj(this->D);
    for(uint i = 0; i < std::pow(2.0, static_cast<int>(this->D)); i++) {
        std::list< vertex<T> > a;

        for(uint j = 0; j < this->D; j++) {
            vals[j] = (((i >> j) & 0x1) ? 1 : -1);
        }

        for(uint j = 0; j < this->D; j++) {
            adj = vals;
            adj[j] *= -1;

            vertex<T> vadj(adj);
            a.push_back(vadj);
        }

        this->v.push_back(vertex<T>(vals));
        this->adj.push_back(a);
    }
}


template<typename T>
inline
pyramoid<T>::pyramoid(uint n) 
    : object<T>(n)
{
    { // add top vertex   
        std::vector<T> vals;
        
        for(uint j = 0; j < this->D; j++) {
            vals.push_back(j == 1 ? 1 : 0);
        }
        
        std::list< vertex<T> > a; 
        vertex<T> vertex(vals);
        this->v.push_back(vertex);
        this->adj.push_back(a);
    }

    for(uint i = 0; i < std::pow(2.0, static_cast<int>(this->D)); i++) {
        std::vector<T> vals;
        
        for(uint j = 0; j < this->D; j++) {
            vals.push_back(((i >> j) & 0x1) ? 1 : -1);
        }

        if(vals[1] == 1) 
            continue;
        
        std::list< vertex<T> > a; a.push_back(this->v[0]);
        vertex<T> vertex(vals);
        this->v.push_back(vertex);
        this->adj.push_back(a);
        this->adj[0].push_back(vertex);
    }

    for(uint i = 1; i < this->size(); i++) {
        std::list< vertex<T> > a;
        vertex<T>& vi = this->v[i];
        
        for(uint j = 1; j < this->size(); j++) {
            vertex<T>& vj = this->v[j];
            int d = 0;

            for(uint k = 0; k < this->D; k++) {
                if(vi[k] != vj[k])
                    d++;
            }

            if(d == 1) {
                this->adj[i].push_back(vj);
            }
        }
    }

}


template<typename T>
inline
spheroid<T>::spheroid(uint n) 
    : object<T>(n)
{
    const T r2 = std::sqrt(static_cast<T>(this->D));
    const T nr2 = -1 * r2;

    // draw longitude lines from +R Y axis through each of the others down to -R


    // draw latitude lines with Y zero using each pair of axes 


    for(uint i = 0; i < this->D; i++) {
        {
            // add top vertex   
            std::vector<T> vals;
            
            for(uint j = 0; j < this->D; j++) {
                vals.push_back(j == i ? r2 : 0);
            }
            
            std::list< vertex<T> > a; 
            vertex<T> vertex(vals);
            this->v.push_back(vertex);
            this->adj.push_back(a);
        }
        
        {
            // add bottom vertex   
            std::vector<T> vals;
            for(uint j = 0; j < this->D; j++) {
                vals.push_back(j == i ? nr2 : 0);
            }
            
            std::list< vertex<T> > a; 
            vertex<T> vertex(vals);
            this->v.push_back(vertex);
            this->adj.push_back(a);
        }
    }

    for(uint i = 0; i < std::pow(2.0, static_cast<int>(this->D)); i++) {
        std::vector<T> vals;
        
        for(uint j = 0; j < this->D; j++) {
            vals.push_back(((i >> j) & 0x1) ? 1 : -1);
        }

        for(uint k = 0; k < this->D; k++) {
            if(vals[k] == 1) {
                std::list< vertex<T> > a; a.push_back(this->v[2*k]);
                vertex<T> vertex(vals);
                this->v.push_back(vertex);
                this->adj.push_back(a);
                this->adj[2*k].push_back(vertex);
            } else if(vals[k] == -1) {
                std::list< vertex<T> > a; a.push_back(this->v[2*k + 1]);
                vertex<T> vertex(vals);
                this->v.push_back(vertex);
                this->adj.push_back(a);
                this->adj[2*k + 1].push_back(vertex);
            }
        }
    }
}

template<typename T>
inline
staroid<T>::staroid(uint n) 
    : object<T>(n)
{
    const T r2 = 2 * std::sqrt(static_cast<T>(this->D));
    const T nr2 = -1 * r2;

    // draw longitude lines from +R Y axis through each of the others down to -R


    // draw latitude lines with Y zero using each pair of axes 


    for(uint i = 0; i < this->D; i++) {
        {
            // add top vertex   
            std::vector<T> vals;
            
            for(uint j = 0; j < this->D; j++) {
                vals.push_back(j == i ? r2 : 0);
            }
            
            std::list< vertex<T> > a; 
            vertex<T> vertex(vals);
            this->v.push_back(vertex);
            this->adj.push_back(a);
        }
        
        {
            // add bottom vertex   
            std::vector<T> vals;
            for(uint j = 0; j < this->D; j++) {
                vals.push_back(j == i ? nr2 : 0);
            }
            
            std::list< vertex<T> > a; 
            vertex<T> vertex(vals);
            this->v.push_back(vertex);
            this->adj.push_back(a);
        }
    }

    for(uint i = 0; i < std::pow(2.0, static_cast<int>(this->D)); i++) {
        std::vector<T> vals;
        
        for(uint j = 0; j < this->D; j++) {
            vals.push_back(((i >> j) & 0x1) ? 1 : -1);
        }

        for(uint k = 0; k < this->D; k++) {
            if(vals[k] == 1) {
                std::list< vertex<T> > a; a.push_back(this->v[2*k]);
                vertex<T> vertex(vals);
                this->v.push_back(vertex);
                this->adj.push_back(a);
                this->adj[2*k].push_back(vertex);
            } else if(vals[k] == -1) {
                std::list< vertex<T> > a; a.push_back(this->v[2*k + 1]);
                vertex<T> vertex(vals);
                this->v.push_back(vertex);
                this->adj.push_back(a);
                this->adj[2*k + 1].push_back(vertex);
            }
        }
    }
}


template<typename T>    
inline
object<T> operator*(const matrix<T>& a, const object<T>& b) {
    object<T> ret(b);

    return ret;
}


}
}  


#endif
