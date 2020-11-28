/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 2020 Joe Yandle <dragon@dancingdragon.net>
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

#ifndef JLIB_MATH_POLYNOMIAL_HH
#define JLIB_MATH_POLYNOMIAL_HH

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

namespace jlib {
namespace math {

template<typename T>
struct Power {
    static T pow(const T& x, const T& y) {
        return std::pow(x, y);
    }
};
    
// represent a polynomial by a vec of its parameters
template<typename T, typename P = Power<T>>
class Polynomial {
public:
    Polynomial();
    Polynomial(const std::vector<T>& params);

    //T& operator[](int i);
    //const T& operator[](int i) const;
    T operator[](int i) const;

    T operator()(const T& x) const;

    Polynomial<T,P> operator+(const Polynomial& p);
    Polynomial<T,P> operator*(const Polynomial& p);

    template<typename U, typename Q>
    friend std::ostream& operator<<(std::ostream& out, const Polynomial<U,Q>& p);
    
    template<typename U, typename Q>
    friend bool operator==(const Polynomial<U,Q>& a, const Polynomial<U,Q>& b);
    template<typename U, typename Q>
    friend bool operator!=(const Polynomial<U,Q>& a, const Polynomial<U,Q>& b);
    
protected:
    std::vector<T> m_params;
};

template<typename T, typename P>    
std::ostream& operator<<(std::ostream& out, const Polynomial<T,P>& p);

template<typename T, typename P>
bool operator==(const Polynomial<T,P>& a, const Polynomial<T,P>& b);

template<typename T, typename P>
bool operator!=(const Polynomial<T,P>& a, const Polynomial<T,P>& b);
    
template<typename T, typename P>
Polynomial<T,P>::Polynomial() {}

template<typename T, typename P>
Polynomial<T,P>::Polynomial(const std::vector<T>& params)
    : m_params(params)
{
}
    /*
template<typename T, typename P>
T& Polynomial<T,P>::operator[](int i) {
    if(i >= m_params.size()) {
        std::cout << *this << std::endl;
        std::cout << "asking for index " << i << " resize to " << (i+1) << std::endl;
        m_params.resize(i+1);
        std::cout << *this << std::endl;
    }
        
    return m_params.at(i);
}
    */
template<typename T, typename P>
//const T& Polynomial<T,P>::operator[](int i) const {
T Polynomial<T,P>::operator[](int i) const {
    if(i < m_params.size())
        return m_params[i];
    else
        return 0;
}

template<typename T, typename P>
T Polynomial<T,P>::operator()(const T& x) const {
    T sum = 0;
    for(int i = 0; i < m_params.size(); i++) {
        //std::cout << "m_params[" << i << "] = " << m_params[i] << std::endl;
        if(i > 0)
            sum += (m_params[i] * P::pow(x, i));
        else 
            sum += (m_params[i]);

        //std::cout << "sum = " << sum << std::endl;
    }
    return sum;
}

template<typename T, typename P>
Polynomial<T,P> Polynomial<T,P>::operator+(const Polynomial<T,P>& p) {
    const std::vector<T>& x = m_params;
    const std::vector<T>& y = p.m_params;
    
    std::vector<T> z(std::max(x.size(), y.size()), T(0));

    for(int i = 0; i < z.size(); i++) {
        z[i] = (*this)[i] + p[i];
    }
    
    return Polynomial<T>(z);
}

template<typename T, typename P>
Polynomial<T,P> Polynomial<T,P>::operator*(const Polynomial<T,P>& p) {
    const std::vector<T>& x = m_params;
    const std::vector<T>& y = p.m_params;
    
    if(x.empty()) return p;
    if(y.empty()) return *this;

    std::vector<T> z(x.size() + y.size() - 1, T(0));

    for(int i = 0; i < x.size(); i++) {
        for(int j = 0; j < y.size(); j++) {
            int k = i + j;
            z[k] += x[i] * y[j];
        }
    }

    // reduce by removing trailing zeroes
    while(!z.empty() && z.back() == T(0))
        z.pop_back();
    
    return Polynomial<T,P>(z);
}

template<typename T, typename P>
std::ostream& operator<<(std::ostream& out, const Polynomial<T,P>& p) {
    for(int i = 0; i < p.m_params.size(); i++) {
        //int k = (p.m_params.size() - 1) - i;
        if(p.m_params[i] == T(0))
            continue;

        if(i == 0) 
            out << p.m_params[i];
        else if(p.m_params[i] == -T(1))
            out << "-";
        else if(p.m_params[i] != T(1))
            out << p.m_params[i];

        if(i > 0 && p.m_params[i] != T(1) && p.m_params[i] != -T(1)) {
            out << " ";
        }
            
        if(i > 1) {
            out << "x^" << i;
        }
        else if(i == 1) {
            out << "x";
        }

        if(i < (p.m_params.size() - 1)) {
            out << " + ";
        }
    }

    return out;
}

template<typename T, typename P>
bool operator==(const Polynomial<T,P>& a, const Polynomial<T,P>& b) {
    return (a.m_params == b.m_params);
}

template<typename T, typename P>
bool operator!=(const Polynomial<T,P>& a, const Polynomial<T,P>& b) {
    return !(a == b);
}
    
}
}

#endif
