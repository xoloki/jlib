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

    // string_value, int_value, double_value.
    //
    // These had no coverage at all despite ~100 call sites, which is how the
    // implementation underneath them stayed at its 2001 shape -- a runtime
    // format string snprintf'd into two heap buffers per call -- behind
    // camelCase forwarders that nobody read.  The padding is the part worth
    // pinning: printf's "%0Nd" puts the zeros *after* the sign.
    struct { int i; int n; const char* want; } ints[] = {
        {      42, -1,      "42" },
        {      42,  0,      "42" },
        {      42,  5,   "00042" },
        {      42,  2,      "42" },   // already wide enough
        {      42,  1,      "42" },   // never truncates
        {     -42,  5,   "-0042" },   // padding after the sign
        {       0,  3,     "000" },
        { -123456,  3, "-123456" },
    };

    for(auto& t : ints) {
        if(string_value(t.i, t.n) != t.want) {
            std::cerr << "error: string_value(" << t.i << "," << t.n << ") == \""
                      << string_value(t.i, t.n) << "\", want \"" << t.want << "\""
                      << std::endl;
            exit(1);
        }
    }

    if(string_value(7u, 3) != "007") {
        std::cerr << "error: string_value(unsigned) padding: "
                  << string_value(7u, 3) << std::endl;
        exit(1);
    }

    // to_string(double) is "%f" -- six decimals -- which is what the snprintf
    // this replaced produced, so the width counts the whole thing.
    if(string_value(3.5) != "3.500000") {
        std::cerr << "error: string_value(double): " << string_value(3.5) << std::endl;
        exit(1);
    }

    if(string_value(3.5, 10) != "003.500000") {
        std::cerr << "error: string_value(double) padding: "
                  << string_value(3.5, 10) << std::endl;
        exit(1);
    }

    if(int_value("42") != 42 || int_value("-7") != -7) {
        std::cerr << "error: int_value" << std::endl;
        exit(1);
    }

    if(int_value("ff", 16) != 255 || int_value("0", 10) != 0) {
        std::cerr << "error: int_value with a base" << std::endl;
        exit(1);
    }

    // strtol semantics, kept deliberately: a string that is not a number is
    // zero rather than an error, and every caller was written against that.
    if(int_value("not a number") != 0) {
        std::cerr << "error: int_value on junk should be 0" << std::endl;
        exit(1);
    }

    if(double_value("2.5") != 2.5 || double_value("junk") != 0.0) {
        std::cerr << "error: double_value" << std::endl;
        exit(1);
    }

    exit(0);
}
