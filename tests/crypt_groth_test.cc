/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joey Yandle <dragon@dancingdragon.net>
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

#include <jlib/crypt/groth.hh>

#include <chrono>

using namespace jlib::crypt::curve;
using namespace jlib::crypt::groth;

int main(int argc, char** argv) {
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
