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

// represent a polynomial by a vec of its parameters
template<typename T>
class Polynomial {
public:
    Polynomial();
    Polynomial(const std::vector<T>& params);

    T& operator[](int i);
    const T& operator[](int i) const;

    Polynomial<T> operator+(const Polynomial& p);
    Polynomial<T> operator*(const Polynomial& p);

    
protected:
    std::vector<T> m_params;
};

template<typename T>    
std::ostream& operator<<(std::ostream& out, const Polynomial<T>& p);

template<typename T>
Polynomial<T>::Polynomial() {}

template<typename T>
Polynomial<T>::Polynomial(const std::vector<T>& params)
    : m_params(params)
{
}
    
template<typename T>
T& Polynomial<T>::operator[](int i) {
    return m_params[i];
}

template<typename T>
const T& Polynomial<T>::operator[](int i) const {
    return m_params[i];
}

template<typename T>
Polynomial<T> Polynomial<T>::operator+(const Polynomial<T>& p) {
    const std::vector<T>& x = m_params;
    const std::vector<T>& y = p.m_params;
    
    std::vector<T> z(std::max(x.size(), y.size()), T(0));

    return Polynomial<T>(z);
}

template<typename T>
Polynomial<T> Polynomial<T>::operator*(const Polynomial<T>& p) {
    const std::vector<T>& x = m_params;
    const std::vector<T>& y = p.m_params;
    
    if(x.empty()) return p;
    if(y.empty()) return *this;

    std::vector<T> z(x.size() + y.size() - 1, T(0));

    for(int i = 0; i < x.size(); i++) {
        for(int j = 0; j < y.size(); j++) {
            int k = i + j;
            z[k] += x[i] + y[j];
        }
    }

    return Polynomial<T>(z);
}

template<typename T>    
std::ostream& operator<<(std::ostream& out, const Polynomial<T>& p) {


    return out;
}

}
}

#endif
