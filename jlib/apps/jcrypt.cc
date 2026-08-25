/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2008 Joey Yandle <xoloki@gmail.com>
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

