/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 2020 Joey Yandle <dragon@dancingdragon.net>
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
#include <jlib/math/polynomial.hh>
#include <jlib/util/util.hh>

namespace jlib {
namespace crypt {
namespace groth {

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

typedef math::Polynomial<curve::Scalar, curve::Scalar::Power> Polynomial;
    
ZeroProof prove(const std::vector<curve::Commitment>& c, std::size_t l, const curve::Scalar& r) {
    ZeroProof proof;

    proof.c = c;

    // find the closest power of 2 to c's size
    double lg = std::log2(c.size());
    double lg_ceil = std::ceil(lg);
    std::size_t n = lg_ceil;
    std::size_t size = std::pow(2, n);

    std::cout << "proving one of " << c.size() << " commitments is zero, log2 of size is " << lg_ceil << "(" << lg << ")" << std::endl;
    
    // expand c if necessary
    if(size > c.size()) {
        proof.c.resize(size, c.back());
    }

    // wait till we resize before setting N
    const std::size_t N = proof.c.size();
    
    // split l into n bits
    std::bitset<64> l_j(l);

    std::vector<curve::Scalar> r_j, a, s, t, rho;

    // in the proof, j goes from 1 to n, while k goes from 0 to (n-1)
    // because this is code not math we always go from 0 to (n-1)
    for(int j = 0; j < n; j++) {
        r_j.push_back(curve::Scalar::random());
        a.push_back(curve::Scalar::random());
        s.push_back(curve::Scalar::random());
        t.push_back(curve::Scalar::random());
        rho.push_back(curve::Scalar::random());

        curve::Scalar ljj = l_j[j] ? curve::Scalar::one() : curve::Scalar::zero();
        
        proof.c_l.push_back(curve::Commitment(ljj, r_j.back()));
        proof.c_a.push_back(curve::Commitment(a.back(), s.back()));

        curve::Scalar cbj = l_j[j] ? a.back() : curve::Scalar::zero();
        
        proof.c_b.push_back(curve::Commitment(cbj, t.back()));
    }

    // now that we have a we can expand f_j,i_j to get p_i(x)
    // section 2.3 of groth paper, equation (1)
    std::vector<Polynomial> p_x;
    for(std::size_t i = 0; i < N; i++) {
        Polynomial p_i_x;
        std::bitset<64> i_j(i);
        // expand the polynomial by multiplying by each f_j,i_j
        for(int j = 0; j < n; j++) {
            Polynomial f_j_i_j = i_j[j] ?
                (l_j[j] ? std::vector<curve::Scalar>{ a[j], curve::Scalar::one() } : std::vector<curve::Scalar> { a[j] }) :
                (!l_j[j] ? std::vector<curve::Scalar>{ -a[j], curve::Scalar::one() } : std::vector<curve::Scalar> { -a[j] });

            p_i_x = (p_i_x * f_j_i_j);
        }

        p_x.push_back(p_i_x);

        std::cout << "p[" << i << "](x) = " << p_x[i] << std::endl;
    }

    // now that we have all p_i(x) we can finally calculate c_d
    for(int j = 0; j < n; j++) {
        curve::Point c_0_rho_k = curve::Commitment(0, rho[j]);

        curve::Point c_d_k = curve::Point::zero(); //proof.c[0] * p_x[0][j];
        for(std::size_t i = 0; i < N; i++) {
            curve::Scalar p_x_i_j = p_x[i][j];
            c_d_k += (proof.c[i] * p_x_i_j);
        }

        c_d_k += c_0_rho_k;

        proof.c_d.push_back(c_d_k);
    }

    curve::Hash<curve::Scalar::HASHSIZE> xhash;
    for(curve::Commitment i : proof.c)
        xhash.update(i);
    for(curve::Commitment i : proof.c_a)
        xhash.update(i);
    for(curve::Commitment i : proof.c_b)
        xhash.update(i);
    for(curve::Point i : proof.c_d)
        xhash.update(i);
    xhash.finalize();
    
    curve::Scalar x = xhash;

    std::cout << "proof hash is " << x << std::endl;

    for(int i = 0; i < N; i++) {
        std::cout << "p[" << i << "](" << x << ") = " << p_x[i](x) << std::endl;
    }
    
    for(int j = 0; j < n; j++) {
        curve::Scalar f_j = l_j[j] ?
            (x + a[j]) : a[j];
        proof.f.push_back(f_j);

        curve::Scalar z_a = r_j[j] * x + s[j];
        proof.z_a.push_back(z_a);

        curve::Scalar z_b = r_j[j] * (x - f_j) + t[j];
        proof.z_b.push_back(z_b);
    }

    proof.z_d = r * (x^n);
    for(int k = 0; k < n; k++) {
        proof.z_d -= (rho[k] * (x^k));
    }

    // if instead of subtracting each, can we make the sum then subtract once?
    curve::Scalar z_d = r * (x^n);
    curve::Scalar S_rhok_xk = curve::Scalar::zero();
    for(int k = 0; k < n; k++) {
        S_rhok_xk += (rho[k] * (x^k));
    }
    z_d -= S_rhok_xk;
    
    std::cout << "proof.z_d = " << proof.z_d << std::endl;
    std::cout << "z_d = " << z_d << std::endl;
    std::cout << "rx^n - S_rhok_xk = " << (r*(x^n) - S_rhok_xk) << std::endl;

    
    // test to see if the proof elements verify in the reduced form
    curve::Point c0zd = curve::Commitment(curve::Scalar::zero(), z_d);

    curve::Point c0rxn = curve::Commitment(curve::Scalar::zero(), r*(x^n));

    curve::Point P_c0rhok_xk = curve::Commitment(curve::Scalar::zero(), rho[0]);
    for(int k = 1; k < n; k++) {
        P_c0rhok_xk += curve::Commitment(curve::Scalar::zero(), rho[k]*(x^(k)));
    }

    //P_c0rhok_xnk = (-curve::Scalar::one()) * P_c0rhok_xnk;

    curve::Point c0_S_rhok_xk = curve::Commitment(0, S_rhok_xk);
    
    std::cout << "c0zd = " << c0zd << std::endl;
    std::cout << "c0rxn = " << c0rxn << std::endl;
    std::cout << "S_rhok_xk = " << S_rhok_xk << std::endl;
    std::cout << "c0_S_rhok_xk = " << c0_S_rhok_xk << std::endl;
    std::cout << "S_rhok_xk * H = " << S_rhok_xk * curve::Commitment::H << std::endl;
    std::cout << "P_c0rhok_xk = " << P_c0rhok_xk << std::endl;
    std::cout << "c0rxn - P_c0rhok_xk = " << (c0rxn - P_c0rhok_xk) << std::endl;
    std::cout << "c0rxn - c0_S_rhok_xk = " << (c0rxn - c0_S_rhok_xk) << std::endl;
    std::cout << "c0_rxn__S_rhok_xk = " << curve::Commitment(0, ((r*(x^n)) - S_rhok_xk)) << std::endl;
    std::cout << "cS0_rxn__S_rhok_xk = " << curve::Commitment(curve::Scalar::zero(), ((r*(x^n)) - S_rhok_xk)) << std::endl;
    
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
    for(curve::Point i : proof.c_d)
        xhash.update(i);
    xhash.finalize();
                     
    curve::Scalar x = xhash;
    const std::size_t n = proof.c_a.size();
    const std::size_t N = proof.c.size();
    
    std::cout << "verify hash is " << x << std::endl;

    for(int j = 0; j < n; j++) {
        curve::Point cxca = x * proof.c_l[j] + proof.c_a[j];
        curve::Point cfza = curve::Commitment(proof.f[j], proof.z_a[j]);
        
        curve::Point cxfcb = (x-proof.f[j])*proof.c_l[j] + proof.c_b[j];
        curve::Point czzb = curve::Commitment(curve::Scalar::zero(), proof.z_b[j]);
        
        if(!(cxca == cfza && cxfcb == czzb)) {
            std::cerr << "groth zeroproof failed to verify because bit proof " << j << " failed" << std::endl;
            return false;
        }
    }

    // first calculate the c_d_k^-x^k product
    //curve::Scalar x_k_0 = curve::Scalar::one();
    //curve::Point c_d_k_x_k_0 = proof.c_d[0] * (-x_k_0);
    curve::Point P_c_d_k_x_k = curve::Point::zero();//c_d_k_x_k_0;
    for(int k = 0; k < n; k++) {
        curve::Point c_d_k_x_k = proof.c_d[k] * (-(x^k));

        P_c_d_k_x_k += c_d_k_x_k;
    }

    curve::Point c0zd = curve::Commitment(curve::Scalar::zero(), proof.z_d);
    /*
    // calculate f_j,i_j starting with i=0
    curve::Scalar P_f_j_i_j_0 = x - proof.f[0];
    for(int j = 1; j < n; j++) {
        // f_j,i_j is always f_j,0 when i=0
        curve::Scalar f_j_i_j = x - proof.f[j];

        P_f_j_i_j_0 *= f_j_i_j;
    }
    */
    curve::Point P_c_i_f_j_i_j = curve::Point::zero();//proof.c[0] * P_f_j_i_j_0;
    for(int i = 0; i < N; i++) {
        std::bitset<64> i_j(i);
        //std::cout << "at i = " << i << ": i_j = " << i_j << std::endl;
        curve::Scalar P_f_j_i_j = curve::Scalar::one();
        for(int j = 0; j < n; j++) {
            curve::Scalar f_j_i_j = i_j[j] ? proof.f[j] : (x - proof.f[j]);
            //std::cout << "i_j[" << j << "] = " << i_j[j] << std::endl;
            P_f_j_i_j *= f_j_i_j;
        }
        
        std::cout << "P_f_j_i_j[" << i << "] = " << P_f_j_i_j << std::endl;

        P_c_i_f_j_i_j += (proof.c[i] * P_f_j_i_j);
    }

    if((P_c_i_f_j_i_j + P_c_d_k_x_k) != c0zd) {
        std::cerr << "groth zeroproof failed to verify because product part failed" << std::endl;
        std::cerr << "P_c_i_f_j_i_j = \n" << P_c_i_f_j_i_j << std::endl;
        std::cerr << "P_c_d_k_x_k = \n" << P_c_d_k_x_k << std::endl;
        std::cerr << "P_c_i_f_j_i_j + P_c_d_k_x_k = \n" << (P_c_d_k_x_k + P_c_i_f_j_i_j) << std::endl;
        std::cerr << "c0zd = \n" << c0zd << std::endl;

        return false;
    }
    
    return true;
}
    
}
}
}
