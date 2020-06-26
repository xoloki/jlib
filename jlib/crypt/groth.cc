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

#include <bitset>
#include <cmath>
#include <ostream>

#include <sodium.h>
#include <sodium/crypto_core_ristretto255.h>

#include <jlib/crypt/groth.hh>
#include <jlib/util/util.hh>

namespace jlib {
namespace crypt {
namespace groth {

std::vector<curve::Scalar> expand(const std::vector<curve::Scalar>& x, const std::vector<curve::Scalar>& y) {
    if(x.empty()) return y;
    if(y.empty()) return x;

    std::vector<curve::Scalar> p(x.size() + y.size() -1, curve::Scalar::zero());

    for(int i = 0; i < x.size(); i++) {
        for(int j = 0; j < y.size(); j++) {
            int k = i + j;
            p[k] += x[i] + y[j];
        }
    }

    return p;
}
        
    
BinaryProof prove(const curve::Scalar& m, const curve::Scalar& r) {
    curve::Commitment c(m, r);
    
    curve::Scalar a = curve::Scalar::random();
    curve::Scalar s = curve::Scalar::random();
    curve::Scalar t = curve::Scalar::random();
    
    curve::Commitment c_a(a, s);
    curve::Commitment c_b(a*m, t);
    
    curve::Scalar x = curve::hash<curve::Scalar::HASHSIZE>(c, c_a, c_b);
    
    curve::Scalar f = m*x + a;
    curve::Scalar z_a = r*x + s;
    curve::Scalar z_b = r*(x - f) + t;

    return BinaryProof{c, c_a, c_b, f, z_a, z_b};
}

bool verify(const BinaryProof& proof) {
    curve::Scalar x = curve::hash<curve::Scalar::HASHSIZE>(proof.c, proof.c_a, proof.c_b);

    curve::Point cxca = x * proof.c + proof.c_a;
    curve::Point cfza = curve::Commitment(proof.f, proof.z_a);
    
    curve::Point cxfcb = (x-proof.f)*proof.c + proof.c_b;
    curve::Point czzb = curve::Commitment(curve::Scalar::zero(), proof.z_b);
    
    return (cxca == cfza && cxfcb == czzb);
}

ZeroProof prove(const std::vector<curve::Commitment>& c, std::size_t l, const curve::Scalar& r) {

    ZeroProof proof;

    proof.c = c;
    
    // find the closest power of 2 to c's size
    double lg = std::log2(c.size());
    double lg_ceil = std::ceil(lg);
    std::size_t n = lg_ceil;
    std::size_t size = std::pow(2, n);
    
    // expand c if necessary
    if(size > c.size()) {
        proof.c.resize(size, c.back());
    }

    // wait till we resize before setting N
    const std::size_t N = proof.c.size();
    
    // split l into n bits
    std::bitset<sizeof(l)> l_j(l);

    std::vector<curve::Scalar> rj, aj, sj, tj, rhoj;
    
    for(int j = 0; j < n; j++) {
        rj.push_back(curve::Scalar::random());
        aj.push_back(curve::Scalar::random());
        sj.push_back(curve::Scalar::random());
        tj.push_back(curve::Scalar::random());
        rhoj.push_back(curve::Scalar::random());

        curve::Scalar ljj = l_j[j] ? curve::Scalar::one() : curve::Scalar::zero();
        
        proof.c_l.push_back(curve::Commitment(ljj, rj.back()));
        proof.c_a.push_back(curve::Commitment(aj.back(), sj.back()));

        curve::Scalar cbj = l_j[j] ? aj.back() : curve::Scalar::zero();
        
        proof.c_b.push_back(curve::Commitment(cbj, sj.back()));
        //proof.c_d.push_back(curve::Commitment(a.back(), s.back()));
    }

    // now that we have aj we can expand f_j,i_j to get p_i(x)
    // treat a polynomial as a vector of scalars representing the coefficients
    std::vector<std::vector<curve::Scalar>> p_x;
    for(std::size_t i = 0; i < N; i++) {
        std::vector<curve::Scalar> p_i_x;
        std::bitset<sizeof(i)> i_j(i);
        // expand the polynomial by multiplying by each f_j,i_j
        for(int j = 0; j < n; j++) {
            std::vector<curve::Scalar> f_j_i_j = i_j[j] ?
                (l_j[j] ? std::vector<curve::Scalar>{ curve::Scalar::one(), aj[j] } : std::vector<curve::Scalar> { curve::Scalar::zero(), aj[j] }) :
                (!l_j[j] ? std::vector<curve::Scalar>{ curve::Scalar::one(), -aj[j] } : std::vector<curve::Scalar> { curve::Scalar::zero(), -aj[j] });

            p_i_x = expand(p_i_x, f_j_i_j);
        }

        p_x.push_back(p_i_x);
    }

    // now that we have all p_i(x) we can finally calculate c_d
    for(int j = 0; j < n; j++) {
        curve::Point c_d_k = proof.c[0] * p_x[0][j];
        for(std::size_t i = 1; i < N; i++) {
            c_d_k += proof.c[i] * p_x[i][j];
        }

        proof.c_d.push_back(c_d_k);
    }
    
    
    curve::Hash<curve::Scalar::HASHSIZE> xhash;
    for(curve::Commitment i : proof.c)
        xhash.update(i);
    for(curve::Commitment i : proof.c_a)
        xhash.update(i);
    for(curve::Commitment i : proof.c_b)
        xhash.update(i);
    xhash.finalize();
    
    curve::Scalar x = xhash;

    return proof;
}
    
bool verify(const ZeroProof& proof) {
    curve::Hash<curve::Scalar::HASHSIZE> xhash;
    for(curve::Commitment c : proof.c)
        xhash.update(c);
    for(curve::Commitment i : proof.c_a)
        xhash.update(i);
    for(curve::Commitment i : proof.c_b)
        xhash.update(i);
    xhash.finalize();
                     
    curve::Scalar x = xhash;

    return false;
}
    
}
}
}
