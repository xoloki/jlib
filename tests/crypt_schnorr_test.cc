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

#include <jlib/crypt/schnorr.hh>

using namespace jlib::crypt::schnorr;
using namespace jlib::crypt::curve;

int main(int argc, char** argv) {
    BasePoint G;
    Scalar x = Scalar::random();
    Point y = x * G;
    Proof p = prove(G, y, x);
    
    if(!verify(p)) {
        std::cerr << "schnorr proof didn't verify" << std::endl;
        return -1;
    } 
    
    return 0;
}
