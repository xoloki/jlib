/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2021 Joey Yandle <xoloki@gmail.com>
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

#include <jlib/crypt/curve.hh>

using namespace jlib::crypt;


int main(int argc, char** argv) {
    // libsodium requires this before any other call into it.
    if(sodium_init() < 0) {
        std::cerr << "sodium_init() failed" << std::endl;
        return -1;
    }

    try {
        
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    } catch(...) {
        std::cerr << "unknown exception" << std::endl;
        return 1;
    }

    return 0;
}

