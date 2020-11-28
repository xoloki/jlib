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

#include <jlib/math/math.hh>
#include <jlib/math/polynomial.hh>
#include <jlib/util/util.hh>

using jlib::math::matrix;
using jlib::math::cuboid;
using jlib::math::vertex;
using jlib::math::Polynomial;
using jlib::util::tokenize;
using jlib::util::int_value;
using jlib::util::double_value;

int main(int argc, char** argv) {
    std::string ss[] = {
	"2 3\n"
	"4 0", 

	"10 15\n"
	"20 0", 

	"1 2 0\n"
	"5 -1 0",

	"17 1 0\n"
	"4 8 0",

	"1 1 1\n"
	"1 1 3\n"
	"2 5 8",

	"0 0 1\n"
	"1 1 3\n"
	"0 5 8",

	"0 1\n"
	"1 2" };

    uint n = sizeof(ss) / sizeof(std::string);
    std::vector< matrix<int> > m;

    for (uint i = 0; i < n; i++) {
        std::vector<std::string> tok = tokenize(ss[i], "\n", false);

        uint x = tok.size();
        uint y = tokenize(tok[0], " ", false).size();

        std::istringstream is(ss[i]);
        matrix<int> ma(x, y);
        
        is >> ma;
        
        m.push_back(ma);
    }
    
    matrix<int> mi = matrix<int>::identity(2);
    matrix<int> md = matrix<int>::diagonal(2, 5);

    try {
        matrix<int> m4 = (m[0] * m[2]);
        if(m4 != m[3]) {
            std::cerr << m[0] << std::endl << "*" << std::endl
                      << m[2] << std::endl << "=" << std::endl
                      << m4 << std::endl << "rather than" << std::endl
                      << m[3] << std::endl << std::endl;
        }
        
        matrix<int> m5 = (m[0] * mi);
        if(m5 != m[0]) {
            std::cerr << m[0] << std::endl << "*" << std::endl
                      << mi << std::endl << "=" << std::endl
                      << m5 << std::endl << "rather than" << std::endl
                      << m[0] << std::endl << std::endl;
        }
        
        matrix<int> m6 = (m[0] * md);
        if(m6 != m[1]) {
            std::cerr << m[0] << std::endl << "*" << std::endl
                      << md << std::endl << "=" << std::endl
                      << m6 << std::endl << "rather than" << std::endl
                      << m[1] << std::endl << std::endl;
        }
        
        std::map<std::string, matrix<int> > factors = 
            m[5].factor(matrix<int>::LU);
        
        std::cout << factors.find("P")->second << "*" << std::endl 
                  << m[5] << "=" << std::endl 
                  << (m[5] * factors.find("P")->second) << std::endl << std::endl;
        
        std::vector< std::pair<double,double> > clip;
        clip.push_back(std::make_pair(-1.0, 1.0));
        clip.push_back(std::make_pair(-1.0, 1.0));
        clip.push_back(std::make_pair(-1.0, 1.0));
        clip.push_back(std::make_pair(1.0, 4.0));
        matrix<double> tp4 = matrix<double>::project(4, clip);
        std::cout << tp4 << std::endl << std::endl;
        
        cuboid<double> square(2);

        matrix<int> reg(3, 1);

        reg(0, 0) = 0;
        reg(1, 0) = 1;
        reg(2, 0) = 2;
	
        matrix<int> trans = reg.transpose();

        std::cout << reg << std::endl;
        std::cout << trans << std::endl;

        Polynomial<int> x = std::vector<int>{ 1, 1 };
        Polynomial<int> y = std::vector<int>{ 1, 1 };
        Polynomial<int> z = x * y;
        Polynomial<int> zz = z * x;
        Polynomial<int> zzz = zz * x;

        std::cout << "(" << x << ") * (" << y << ") = " << z << std::endl;
        std::cout << "(" << z << ") * (" << x << ") = " << zz << std::endl;
        std::cout << "(" << zz << ") * (" << x << ") = " << zzz << std::endl;

        if(z != Polynomial<int>(std::vector<int>{1, 2, 1})) {
            std::cerr << "first binomial expansion failed" << std::endl;
            return 1;
        }
    
        if(zz != Polynomial<int>(std::vector<int>{1, 3, 3, 1})) {
            std::cerr << "second binomial expansion failed" << std::endl;
            return 1;
        }

        // now try something more complicated
        Polynomial<int> a = std::vector<int>{ -1, 0, 2 };
        Polynomial<int> b = std::vector<int>{ -6, 0, -1 };
        Polynomial<int> c = a * b;
    
        std::cout << "(" << a << ") * (" << b << ") = " << c << std::endl;
        if(c != Polynomial<int>(std::vector<int>{6, 0, -11, 0, -2})) {
            std::cerr << "weird expansion failed" << std::endl;
            return 1;
        }
    
        Polynomial<int> a1 = std::vector<int>{ -1, 2, 1 };
        Polynomial<int> b1 = std::vector<int>{ 6, -3, 2 };
        Polynomial<int> c1 = a1 * b1;
    
        std::cout << "(" << a1 << ") * (" << b1 << ") = " << c1 << std::endl;
        if(c1 != Polynomial<int>(std::vector<int>{-6, 15, -2, 1, 2})) {
            std::cerr << "super weird expansion failed" << std::endl;
            return 1;
        }
        
        int eval0 = a1(0);
        if(eval0 != -1) {
            std::cerr << "(" << a1 << ")(0) = " << eval0 << " not -1" << std::endl;
            return 1;
        }
        
        int eval1 = a1(1);
        if(eval1 != 2) {
            std::cerr << "(" << a1 << ")(1) = " << eval1 << " not -1" << std::endl;
            return 1;
        }

        int eval2 = a1(2);
        if(eval2 != 7) {
            std::cerr << "(" << a1 << ")(2) = " << eval2 << " not -1" << std::endl;
            return 1;
        }

        int eval3 = a1(3);
        if(eval3 != 14) {
            std::cerr << "(" << a1 << ")(3) = " << eval3 << " not -1" << std::endl;
            return 1;
        }


    } catch (matrix<int>::mismatch) {
        std::cerr << "matrices are mismatched" << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "caught error" << std::endl;
        return 1;
    }
    return 0;
}

