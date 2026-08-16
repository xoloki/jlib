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
#include <memory>
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
    
    static matrix<T> project(uint n, const std::vector< std::pair<T,T> >& clip);
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


/**
 * A shape: vertices, and the topology connecting them.
 *
 * Topology is held as indices into the vertex list.  It used to be held as
 * copies of the vertices themselves, which made coordinates the identity of a
 * vertex, and that is wrong in three ways.
 *
 * Two vertices with the same coordinates became indistinguishable, so a
 * renderer looking them up by value collapsed them into one -- which spheroid
 * and staroid trigger constantly, since they emit D*2^D vertex records with
 * heavy duplication.  Every lookup cost a tree search and a coordinate
 * comparison.  And change() had to walk every adjacency list re-dimensioning
 * the copies, which indices make unnecessary.
 *
 * faces holds each face as a loop of vertex indices.  Nothing fills it yet;
 * solid rendering needs it and a 1-skeleton cannot supply it.
 */
template<typename T>
class object {
public:
    /** A face, as a loop of vertex indices. */
    typedef std::vector<uint> face_type;

    /** An edge, as a pair of vertex indices, always ordered low to high. */
    typedef std::pair<uint,uint> edge_type;

    object(uint n);
    virtual ~object() {}

    /**
     * A copy that keeps its actual type.
     *
     * Plot stores objects by value, so handing it a cuboid used to slice off
     * everything but the base -- including build_faces(), which left the copy
     * a 1-skeleton no matter what was added.  Shapes differ only in how they
     * are built, so this is the one thing they need to carry.
     */
    virtual std::shared_ptr< object<T> > clone() const;

    void normalize();
    
    vertex<T>& operator[](uint x);
    const vertex<T>& operator[](uint x) const;
    uint size() const;

    /**
     * Indices of the vertices adjacent to x.
     *
     * Symmetric: if b is adjacent to a then a is adjacent to b, so walking
     * every vertex's neighbours visits each edge twice, once from each end.
     */
    const std::vector<uint>& adjacent(uint x) const;

    /** Each edge once, rather than once per endpoint. */
    const std::vector<edge_type>& get_edges() const;

    /**
     * The faces, built on first use.
     *
     * Deferred rather than built in the constructor because there are a lot
     * of them: an n-cube has C(n,2)*2^(n-2), which is 372736 at n=14 -- a
     * dimensionality wireframe rendering handles comfortably, and four times
     * the vertex count.  Nothing should pay for faces it never draws.
     */
    const std::vector<face_type>& get_faces() const;

    void change(uint n);

    uint D;

protected:
    /** Record an edge between two vertices, in both directions. */
    void connect(uint a, uint b);

    /**
     * Fill faces.  Called at most once, from get_faces().
     *
     * The base has none: a shape is a 1-skeleton unless a subclass says
     * otherwise, and enumerating 2-faces is specific to the shape.
     */
    virtual void build_faces() const {}

    std::vector<vertex<T> > v;
    std::vector< std::vector<uint> > adj;
    std::vector<edge_type> edges;

    // mutable so get_faces() can stay const
    mutable std::vector<face_type> faces;
    mutable bool faces_built;
};


template<typename T>
class cuboid : public object<T> {
public:
    cuboid(uint n);

    virtual std::shared_ptr< object<T> > clone() const;

protected:
    virtual void build_faces() const;
};


/**
 * A flat torus: k circles, one per pair of coordinate axes, in n dimensions.
 *
 * The Cartesian product of k circles, S^1 x ... x S^1, with circle j laid in
 * the (2j, 2j+1) coordinate plane and every axis from 2k up left at zero:
 *
 *     x_2j     = r cos(theta_j)
 *     x_(2j+1) = r sin(theta_j)
 *
 * At k=2, n=4 this is the Clifford torus, which is what XScreenSaver's
 * hypertorus draws.  Every radius is 1/sqrt(k), so the whole thing sits on the
 * unit sphere of R^2k.
 *
 * It is flat -- zero Gaussian curvature -- which is why no such surface exists
 * in three dimensions and why its shadow is so strange.  Rotations that mix
 * the unused axes into the used ones carry parts of it through the axis being
 * projected away, and the perspective divide inflates and deflates them, so
 * the surface passes through itself and comes back out.
 *
 * k is independent of n, so the mesh does not grow when n does: a 2-torus is
 * the same m^2 vertices whether it turns in four dimensions or ten, and only
 * the number of planes it can turn in changes.  n must be at least 2k.
 *
 * The topology is a cuboid's with cycles in place of edges.  A vertex is a
 * tuple of k digits base m rather than a pattern of n bits, its neighbour
 * along axis j steps that digit by one modulo m rather than flipping a bit,
 * and a face still comes from choosing two axes and pinning the rest.
 */
template<typename T>
class torus : public object<T> {
public:
    torus(uint n, uint k = 2, uint m = 32,
          std::vector<T> weight = std::vector<T>());

    virtual std::shared_ptr< object<T> > clone() const;

    /** Circles. */
    uint K;

    /** Samples around each circle. */
    uint M;

    /**
     * Radius of each circle, normalized so the sum of squares is one.
     *
     * Equal radii give the Clifford torus.  Unequal ones give the rest of the
     * family: with k=2 and radii (cos a, sin a), every a in (0, pi/2) is a
     * torus, they are disjoint, and together they fill the sphere except for
     * the two circles a=0 and a=pi/2 that they close down onto.  That is the
     * Hopf foliation, and stereographic projection shows it as tori nested
     * one inside the next, each threading through the last.
     */
    std::vector<T> R;

protected:
    virtual void build_faces() const;
};


template<typename T>
class pyramoid : public object<T> {
public:
    pyramoid(uint n);

    virtual std::shared_ptr< object<T> > clone() const;
};


template<typename T>
class staroid : public object<T> {
public:
    staroid(uint n);

    virtual std::shared_ptr< object<T> > clone() const;
};

template<typename T>
class spheroid : public object<T> {
public:
    spheroid(uint n);

    virtual std::shared_ptr< object<T> > clone() const;
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
matrix<T> matrix<T>::project(uint n, const std::vector< std::pair<T,T> >& clip) {
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
    : D(n),
      faces_built(false)
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
const std::vector<uint>& object<T>::adjacent(uint x) const {
    return adj[x];
}

template<typename T>
inline
const std::vector<typename object<T>::edge_type>& object<T>::get_edges() const {
    return edges;
}

template<typename T>
inline
std::shared_ptr< object<T> > object<T>::clone() const {
    return std::make_shared< object<T> >(*this);
}


template<typename T>
inline
const std::vector<typename object<T>::face_type>& object<T>::get_faces() const {
    if(!faces_built) {
        faces_built = true;
        build_faces();
    }

    return faces;
}

template<typename T>
inline
void object<T>::connect(uint a, uint b) {
    if(a == b)
        return;

    adj[a].push_back(b);
    adj[b].push_back(a);

    edges.push_back(a < b ? edge_type(a, b) : edge_type(b, a));
}

template<typename T>
inline
void object<T>::change(uint n) {
    // Only the vertices: the topology is indices, which do not change with
    // dimensionality.  This used to re-dimension a copy of every vertex held
    // in every adjacency list as well.
    for(uint i = 0; i < size(); i++) {
        v[i].change(n);
    }
}


template<typename T>
inline
std::shared_ptr< object<T> > cuboid<T>::clone() const {
    return std::make_shared< cuboid<T> >(*this);
}


template<typename T>
inline
cuboid<T>::cuboid(uint n) 
    : object<T>(n)
{
    const uint count = 1u << this->D;

    std::vector<T> vals(this->D);
    for(uint i = 0; i < count; i++) {
        for(uint j = 0; j < this->D; j++) {
            vals[j] = (((i >> j) & 0x1) ? 1 : -1);
        }

        this->v.push_back(vertex<T>(vals));
        this->adj.push_back(std::vector<uint>());
    }

    // Vertex i is the bit pattern i, so flipping bit j is the neighbour along
    // axis j.  Only connect upwards, or every edge would be added twice.
    for(uint i = 0; i < count; i++) {
        for(uint j = 0; j < this->D; j++) {
            const uint n = i ^ (1u << j);
            if(i < n)
                this->connect(i, n);
        }
    }
}


/**
 * The 2-faces of an n-cube.
 *
 * A face is fixed by choosing two axes to leave free and pinning the other
 * n-2 coordinates to a particular sign pattern, so there are C(n,2)*2^(n-2)
 * of them: 6 for a cube, 24 for a tesseract, 80 for a 5-cube.
 *
 * Vertex i is the bit pattern i, so a face's four corners are the base
 * pattern with the two free bits taking all four combinations.  They are
 * emitted 00, 10, 11, 01 -- around the square rather than across it, so the
 * loop is a real quad and not a bowtie.
 *
 * No attempt is made to wind them consistently outward.  A hypercube reduced
 * to three dimensions is a shadow, not a solid: faces turn inside out as it
 * rotates, and both sides are seen.  Normals are computed per frame from the
 * projected positions, and lighting is two-sided.
 */
template<typename T>
inline
void cuboid<T>::build_faces() const {
    const uint count = 1u << this->D;

    for(uint a = 0; a < this->D; a++) {
        for(uint b = a + 1; b < this->D; b++) {
            const uint bit_a = 1u << a;
            const uint bit_b = 1u << b;

            for(uint base = 0; base < count; base++) {
                // one face per assignment of the other n-2 coordinates
                if((base & bit_a) != 0 || (base & bit_b) != 0)
                    continue;

                typename object<T>::face_type f;
                f.push_back(base);
                f.push_back(base | bit_a);
                f.push_back(base | bit_a | bit_b);
                f.push_back(base | bit_b);

                this->faces.push_back(f);
            }
        }
    }
}


template<typename T>
inline
std::shared_ptr< object<T> > torus<T>::clone() const {
    return std::make_shared< torus<T> >(*this);
}


template<typename T>
inline
torus<T>::torus(uint n, uint k, uint m, std::vector<T> weight)
    : object<T>(n),
      K(k),
      M(m)
{
    // A cycle needs three vertices to be one: at two the step forward and the
    // step back are the same neighbour, at one it is a self-loop.
    if(M < 3) M = 3;
    if(K < 1) K = 1;

    // Each circle occupies a coordinate pair, so there has to be room.
    if(2 * K > n) K = n / 2;

    uint count = 1;
    for(uint j = 0; j < K; j++) count *= M;

    // On the unit sphere, whatever the proportions: the radii are normalized
    // so their squares sum to one.  Equal radii are the Clifford torus.
    R.assign(K, 1.0 / std::sqrt(static_cast<T>(K)));

    if(weight.size() == K) {
        T sum = 0;
        for(uint j = 0; j < K; j++) sum += weight[j] * weight[j];

        if(sum > 0) {
            sum = std::sqrt(sum);
            for(uint j = 0; j < K; j++) R[j] = weight[j] / sum;
        }
    }

    for(uint i = 0; i < count; i++) {
        vertex<T> v(n);
        for(uint x = 0; x < n; x++) v[x] = 0;

        // digit j of i, base M, is the position around circle j
        uint rest = i;
        for(uint j = 0; j < K; j++) {
            const uint digit = rest % M;
            rest /= M;

            const T angle = 2 * M_PI * static_cast<T>(digit) / M;

            v[2 * j]     = R[j] * std::cos(angle);
            v[2 * j + 1] = R[j] * std::sin(angle);
        }

        this->v.push_back(v);
        this->adj.push_back(std::vector<uint>());
    }

    // One edge per vertex per circle: the step forward.  Stepping back would
    // reach the same edges from the other end.
    uint stride = 1;
    for(uint j = 0; j < K; j++) {
        for(uint i = 0; i < count; i++) {
            const uint digit = (i / stride) % M;
            const uint next = i + stride * ((digit + 1) % M) - stride * digit;

            this->connect(i, next);
        }

        stride *= M;
    }
}


/**
 * The 2-faces of a flat torus.
 *
 * One quad per vertex per pair of circles: step forward along both.  That is
 * C(k,2)*m^k of them -- 1024 for a 2-torus at m=32, and equal to the vertex
 * count, which is Euler characteristic zero as a torus requires.
 *
 * Emitted around the quad rather than across it, and consistently: unlike a
 * hypercube's shadow, this is a real surface, so its faces have a side.
 */
template<typename T>
inline
void torus<T>::build_faces() const {
    uint count = 1;
    for(uint j = 0; j < K; j++) count *= M;

    std::vector<uint> stride(K);
    uint s = 1;
    for(uint j = 0; j < K; j++) { stride[j] = s; s *= M; }

    for(uint a = 0; a < K; a++) {
        for(uint b = a + 1; b < K; b++) {
            for(uint i = 0; i < count; i++) {
                const uint da = (i / stride[a]) % M;
                const uint db = (i / stride[b]) % M;

                const uint ia = i + stride[a] * ((da + 1) % M) - stride[a] * da;
                const uint ib = i + stride[b] * ((db + 1) % M) - stride[b] * db;

                const uint dab = (ia / stride[b]) % M;
                const uint iab = ia + stride[b] * ((dab + 1) % M) - stride[b] * dab;

                typename object<T>::face_type f;
                f.push_back(i);
                f.push_back(ia);
                f.push_back(iab);
                f.push_back(ib);

                this->faces.push_back(f);
            }
        }
    }
}


template<typename T>
inline
std::shared_ptr< object<T> > pyramoid<T>::clone() const {
    return std::make_shared< pyramoid<T> >(*this);
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
        
        vertex<T> vertex(vals);
        this->v.push_back(vertex);
        this->adj.push_back(std::vector<uint>());
    }

    for(uint i = 0; i < (1u << this->D); i++) {
        std::vector<T> vals;
        
        for(uint j = 0; j < this->D; j++) {
            vals.push_back(((i >> j) & 0x1) ? 1 : -1);
        }

        if(vals[1] == 1) 
            continue;
        
        vertex<T> vertex(vals);
        this->v.push_back(vertex);
        this->adj.push_back(std::vector<uint>());

        // apex to base corner
        this->connect(0, this->size() - 1);
    }

    // base corners to each other, where they differ in exactly one coordinate.
    // j starts at i+1 so each edge is recorded once.
    for(uint i = 1; i < this->size(); i++) {
        vertex<T>& vi = this->v[i];
        
        for(uint j = i + 1; j < this->size(); j++) {
            vertex<T>& vj = this->v[j];
            int d = 0;

            for(uint k = 0; k < this->D; k++) {
                if(vi[k] != vj[k])
                    d++;
            }

            if(d == 1) {
                this->connect(i, j);
            }
        }
    }

}


template<typename T>
inline
std::shared_ptr< object<T> > spheroid<T>::clone() const {
    return std::make_shared< spheroid<T> >(*this);
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
            
            vertex<T> vertex(vals);
            this->v.push_back(vertex);
            this->adj.push_back(std::vector<uint>());
        }
        
        {
            // add bottom vertex   
            std::vector<T> vals;
            for(uint j = 0; j < this->D; j++) {
                vals.push_back(j == i ? nr2 : 0);
            }
            
            vertex<T> vertex(vals);
            this->v.push_back(vertex);
            this->adj.push_back(std::vector<uint>());
        }
    }

    for(uint i = 0; i < std::pow(2.0, static_cast<int>(this->D)); i++) {
        std::vector<T> vals;
        
        for(uint j = 0; j < this->D; j++) {
            vals.push_back(((i >> j) & 0x1) ? 1 : -1);
        }

        // A fresh vertex per corner per axis, each joined to one pole.  These
        // duplicate coordinates heavily -- which is exactly what a value-keyed
        // renderer used to collapse.  As indices they stay distinct.
        for(uint k = 0; k < this->D; k++) {
            const uint pole = (vals[k] == 1) ? (2*k) : (2*k + 1);

            vertex<T> vertex(vals);
            this->v.push_back(vertex);
            this->adj.push_back(std::vector<uint>());

            this->connect(pole, this->size() - 1);
        }
    }
}

template<typename T>
inline
std::shared_ptr< object<T> > staroid<T>::clone() const {
    return std::make_shared< staroid<T> >(*this);
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
            
            vertex<T> vertex(vals);
            this->v.push_back(vertex);
            this->adj.push_back(std::vector<uint>());
        }
        
        {
            // add bottom vertex   
            std::vector<T> vals;
            for(uint j = 0; j < this->D; j++) {
                vals.push_back(j == i ? nr2 : 0);
            }
            
            vertex<T> vertex(vals);
            this->v.push_back(vertex);
            this->adj.push_back(std::vector<uint>());
        }
    }

    for(uint i = 0; i < std::pow(2.0, static_cast<int>(this->D)); i++) {
        std::vector<T> vals;
        
        for(uint j = 0; j < this->D; j++) {
            vals.push_back(((i >> j) & 0x1) ? 1 : -1);
        }

        // A fresh vertex per corner per axis, each joined to one pole.  These
        // duplicate coordinates heavily -- which is exactly what a value-keyed
        // renderer used to collapse.  As indices they stay distinct.
        for(uint k = 0; k < this->D; k++) {
            const uint pole = (vals[k] == 1) ? (2*k) : (2*k + 1);

            vertex<T> vertex(vals);
            this->v.push_back(vertex);
            this->adj.push_back(std::vector<uint>());

            this->connect(pole, this->size() - 1);
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
