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

#include <jlib/crypt/groth.hh>

#include <iostream>

#include <chrono>

using namespace jlib::crypt::curve;
using namespace jlib::crypt::groth;

int main(int argc, char** argv) {
    // libsodium requires this before any other call into it.
    if(sodium_init() < 0) {
        std::cerr << "sodium_init() failed" << std::endl;
        return -1;
    }

    {
        Scalar m = Scalar::random();
        Scalar m0 = Scalar::zero();
        Scalar m1 = Scalar::one();
        Scalar r = Scalar::random();

        BinaryProof proof = prove(m, r);
        if(verify(proof)) {
            std::cerr << "groth BinaryProof with random m shouldn't verify" << std::endl;
            return -1;
        }
        BinaryProof proof0 = prove(m0, r);
        if(!verify(proof0)) {
            std::cerr << "groth BinaryProof with m=0 should verify" << std::endl;
            return -1;
        }
        BinaryProof proof1 = prove(m1, r);
        if(!verify(proof1)) {
            std::cerr << "groth BinaryProof with m=1 should verify" << std::endl;
            return -1;
        }
    }
    {
        std::vector<Commitment> cs;
        Scalar m = Scalar::zero();
        Scalar r = Scalar::random();
        Commitment c(m, r);
        int count = 2048;

        if(argc > 1) {
            count = std::stoi(argv[1]);
        }
        
        cs.push_back(c);
        cs.push_back(c);

        count -= 2;
        
        // not quite a power of 2
        for(int i = 0; i < count; i++) {
            Scalar a = Scalar::random();
            Scalar b = Scalar::random();
            Commitment comm(a, b);

            cs.push_back(comm);
        }

        auto proof_start = std::chrono::high_resolution_clock::now();
        ZeroProof proof = prove(cs, 1, r);
        auto proof_stop = std::chrono::high_resolution_clock::now();
        
        std::chrono::milliseconds proof_time = std::chrono::duration_cast<std::chrono::milliseconds>(proof_stop - proof_start);

        std::cout << "ZeroProof prove took " << proof_time.count() << "ms" << std::endl;
            
        
        auto verify_start = std::chrono::high_resolution_clock::now();
        bool success = verify(proof);
        auto verify_stop = std::chrono::high_resolution_clock::now();
        std::chrono::milliseconds verify_time = std::chrono::duration_cast<std::chrono::milliseconds>(verify_stop - verify_start);
        std::cout << "ZeroProof verify took " << verify_time.count() << "ms" << std::endl;
        if(!success) {
            std::cerr << "groth ZeroProof didn't verify" << std::endl;
            return -1;
        }
    }

    {
        std::vector<Commitment> cs;
        Scalar m = Scalar::one();
        Scalar r = Scalar::random();
        Commitment c(m, r);
 
        cs.push_back(c);
        
        // not quite a power of 2
        for(int i = 0; i < 15; i++) {
            Scalar a = Scalar::random();
            Scalar b = Scalar::random();
            Commitment comm(a, b);
            
            cs.push_back(comm);
        }
        
        ZeroProof proof = prove(cs, 0, r);
        if(verify(proof)) {
            std::cerr << "groth ZeroProof verified when it shouldn't" << std::endl;
            return -1;
        }
    }

    
    return 0;
}
