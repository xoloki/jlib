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

#include <jlib/sys/sync.hh>

#include <iostream>

#include <cstdlib>

int global_foo = 0;

class Foo {
public:
    virtual std::string foo() { return "foo"; }
    std::string echo(std::string s) { return s; }
    void set_global_foo(int s) { global_foo = s; }
};

class Bar : public Foo {
public:
    virtual std::string foo() { return "bar"; }
};

int main(int argc, char** argv) {
    using jlib::sys::sync;

    sync<int> i(0);

    if(i != 0) {
        std::cerr << "error: incorrect value for sync: expected 0 got " << (int)i << std::endl;
        exit(1);
    }

    i = 1;

    if(i != 1) {
        std::cerr << "error: incorrect value for sync: expected 1 got " << (int)i << std::endl;
        exit(1);
    }

    exit(0);
}
