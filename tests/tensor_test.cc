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

#include <iostream>
#include <sstream>
#include <string>

#include <jlib/math/tensor.hh>
#include <jlib/util/util.hh>

using namespace jlib::math;

typedef double T;

int main(int argc, char** argv) {
    tensor<T> scalar(0);
    tensor<T> vector(1, 5);
    tensor<T> matrix(2, 5, 5);
    const T VAL = 5.0;
    T val;

    scalar = VAL;
  
    try {
        vector = 5.0;

        std::cerr << "tensor_test: was able to assign a scalar to a vector!" << std::endl;
        return 1;
    } catch(tensor<T>::mismatch&) {}

    val = scalar;
    if(val != VAL) {
        std::cerr << "tensor_test: assigning rank 0 tensor to scalar failed to return assigned value ("<< val << " != " << VAL << ")!" << std::endl;
        return 1;
    }

    return 0;
}

 
