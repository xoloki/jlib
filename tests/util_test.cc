/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2011 Joey Yandle <xoloki@gmail.com>
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
 */

#include <iostream>

#include <jlib/util/util.hh>

#include <cstdlib>

int main(int argc, char** argv) {
    using namespace jlib::util;

    std::string foo("\x00\x01\x02\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",16);
    int intarr[4];
    for(int i=0;i<4;i++) {
        intarr[i] = i;
    }

    if(get<char>(foo) != 0x00) {
        std::cerr << "error: in jlib::util::get<char>(std::string)"<< std::endl;
        exit(1);
    }
    if(get<u_short>(foo,2) != 0x0302) {
        std::cerr << "error: in jlib::util::get<u_short>(std::string,u_int): "
                  << (int)get<u_short>(foo,2) <<std::endl;
        exit(1);
    }
    if(get<u_long>(foo,0) != 0x03020100) {
        std::cerr << "error: in jlib::util::get<u_long>(std::string,u_int): "
                  << (int)get<u_long>(foo,0) <<std::endl;
        exit(1);
    }

    set<char>(foo,0x01);
    if(get<char>(foo) != 0x01) {
        std::cerr << "error: in jlib::util::set<char>(std::string,char): "
                  << (int)get<char>(foo) <<std::endl;
        exit(1);
    }

    set<u_short>(foo, 0xffff, 1);
    if(get<u_short>(foo,1) != 0xffff) {
        std::cerr << "error: in jlib::util::set<u_short>(std::string,char,u_int): "
                  << (int)get<u_short>(foo,1) <<std::endl;
        exit(1);
    }
    
    set<u_long>(foo, 0x06060606, 0);
    if(get<u_long>(foo,0) != 0x06060606) {
        std::cerr << "error: in jlib::util::set<u_long>(std::string,char,u_int): "
                  << (int)get<u_long>(foo,0) <<std::endl;
        exit(1);
    }
    
    copy<int>(foo,intarr,2,4);
    if(get<int>(foo, 8) != 1) {
        std::cerr << "error: in jlib::util::copy<int>(std::string,int*,u_int,u_int): "
                  << hex_value(foo) <<std::endl;
        exit(1);
    }
    
    byte_copy(foo, intarr+3, 1);
    if(get<char>(foo) != 3) {
        std::cerr << "error: in jlib::util::byte_copy<int>(std::string,int*,u_int): "
                  << hex_value(foo) <<std::endl;
        exit(1);
    }

    exit(0);
}
