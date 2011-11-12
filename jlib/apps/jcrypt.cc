/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 2008 Joe Yandle <joey@divisionbyzero.com>
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
#include <unistd.h>

#include <cmath>
#include <cstring>
#include <cstdlib>

#include <jlib/crypt/crypt.hh>

using namespace jlib::crypt;


int main(int argc, char** argv) {

    try {
        std::string id = "joey@divisionbyzero.com";
        if(argc > 1) {
            id = argv[1];
        }

        std::string text = "sex drugs terrorist bomb";
        if(argc > 2) {
            text = argv[2];
        }

        gpg::init(GPGME_PROTOCOL_OpenPGP);

        gpg::ctx ctx;
        ctx.set_armor();
        gpg::key::list l = gpg::list_keys(id);

        std::cout << "Found " << l.size() << " keys for id " << id << std::endl;

        gpg::key::ptr k = l.front();
        std::cout << "Key 1 can_sign: " << k->gpgme()->can_sign << std::endl;
        std::cout << "Key 1 can_encrypt: " << k->gpgme()->can_encrypt << std::endl;
        std::cout << "Key 1 secret: " << k->gpgme()->secret << std::endl;

        gpg::data::ptr plain = gpg::data::create(text);
        gpg::data::ptr cipher = gpg::data::create();

        std::cout << "Plaintext:\n" << text << std::endl;

        ctx.op_encrypt(l, plain, cipher);

        gpgme_encrypt_result_t res = ctx.op_encrypt_result();
        if(!res || res->invalid_recipients) {
            std::cout << "Error: " << (res ? "invalid recipients" : "no result") << std::endl;
        } else {
            std::cout << "All recipients succeeded" << std::endl;
        }

        std::cout << "Ciphertext:\n" << cipher->read() << std::endl;


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

