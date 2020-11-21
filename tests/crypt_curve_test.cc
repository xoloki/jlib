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

using namespace jlib::crypt::curve;

int main(int argc, char** argv) {
    {
        Scalar a = Scalar::one();
        Scalar b = Scalar::zero();
        Scalar c = a + b;

        std::cout << a << " + " << b << " = " << c << std::endl;

        if(c != a) {
            std::cerr << "Adding zero to a Scalar one doesn't give us Scalar one" << std::endl;
            return -1;
        }

        Point z = Point::zero();
        Point p = Point::random();
        Point s = z + p;

        std::cout << z << " + " << p << " = " << s << std::endl;

        if(p != s) {
            std::cerr << "Adding a zero point to a random point doesn't give us the random point back" << std::endl;
            return -1;
        }
    }
    {
        Scalar a = Scalar::random();
        Scalar b = Scalar::one();
        Scalar c = a * b;

        std::cout << a << " * " << b << " = " << c << std::endl;

        if(c != a) {
            std::cerr << "Mutiplying a Scalar by one doesn't give us the Scalar back" << std::endl;
            return -1;
        }
    }
    {
        Scalar a = Scalar::random();
        Scalar a2 = a^4;
        Scalar c = a * a * a * a;

        std::cout << a << " * " << a << " = " << c << std::endl;
        std::cout << a << "^4 " << " = " << a2 << std::endl;

        if(c != a2) {
            std::cerr << "Squaring Scalar doesn't give us the same as multiplying the Scalar by itself" << std::endl;
            return -1;
        }
    }
    {
        Scalar a = Scalar::random();
        Scalar an2 = a^(-2);
        Scalar c = (a * a)^(-1);

        std::cout << a << " * " << a << " = " << c << std::endl;
        std::cout << a << "^(-2) " << " = " << an2 << std::endl;

        if(c != an2) {
            std::cerr << "Raising a Scalar to -2 doesn't give us the same as multiplying the Scalar by itself then inverting" << std::endl;
            return -1;
        }
    }
    {
        Scalar a = Scalar::random();
        Scalar an1 = a^(-1);
        Scalar b = a * an1;

        if(b != Scalar::one()) {
            std::cerr << "Multiplying a Scalar by its multiplicative inverse does not give one" << std::endl;
            return -1;
        }
    }
    {
        Scalar x = Scalar::random();
        Scalar y = Scalar::random();
        Scalar z = x + y;

        std::cout << x << " + " << y << " = " << z << std::endl;

        Point a = Point::random();
        Point b = Point::random();
        Point c = a + b;

        std::cout << a << " + " << b << " = " << c << std::endl;
    }
    {
        Scalar x = Scalar::random();
        Scalar y = Scalar::random();
        Commitment c(x, y);

        std::cout << "c = " << c << std::endl;

        Point c1 = x * Commitment::G + y * Commitment::H;

        std::cout << "c1 = " << c1 << std::endl;

        if(c != c1) {
            std::cerr << "Manually doing commitment returns different result from ctor" << std::endl;
            return -1;
        }

        Commitment c0y(Scalar::zero(), y);
        Point cy0 = y * Commitment::H;

        if(c0y != cy0) {
            std::cerr << "Manually doing commitment to zero returns different result from ctor" << std::endl;
            return -1;
        }

        Commitment c0x(x, Scalar::zero());
        Point cx0 = x * Commitment::G;

        if(c0x != cx0) {
            std::cerr << "Manually doing commitment to blinding zero returns different result from ctor" << std::endl;
            return -1;
        }

    }
    // monero
    {
        BasePoint G;

        // private key
        Scalar a = Scalar::random();
        Scalar b = Scalar::random();

        // public key
        Point A = a * G;
        Point B = b * G;

        std::cout << "keypair (A, B) (" << A << ", " << B << ")" << std::endl;

        Scalar s = Scalar::random();
        Point R = s * G;

        Point aR = a * R;
        Scalar Hs = Hash<Point::HASHSIZE>::generic(aR);

        Point Y = Hs * G + B;

        Scalar x = Hs + b;

        Point Y1 = x;

        if(Y != Y1)
            std::cerr << "points don't match" << std::endl;

        // schnorr proof
        Scalar v = Scalar::random();
        Point t = v * G;

        Scalar c = hash<Scalar::HASHSIZE>(G, Y, t);
        Scalar r = v - c*x;

        Point P = r * G + c * Y;

        if(P != t) {
            std::cerr << "schnorr proof didn't verify" << std::endl;
            return -1;
        } 
    }
    // balance proof
    {
        Scalar s1 = Scalar::random(), s2 = Scalar::random(), s3 = Scalar::random(), v1 = Scalar::one(), v2 = Scalar::one(), v3 = v1 + v2;

        Commitment c1(v1, s1), c2(v2, s2), c3(v3, s3);

        Point s = c3 - c1 - c2;
        Scalar z = s3 - s1 - s2;

        if(s != Commitment(0, z) || s != (Commitment::H * z)) {
            std::cerr << "balance proof didn't verify" << std::endl;
            return -1;
        }
    }

    return 0;
}
