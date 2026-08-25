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

 
