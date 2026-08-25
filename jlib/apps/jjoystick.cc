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

#include <jlib/sys/joystick.hh>
#include <jlib/util/util.hh>
#include <string>

using namespace jlib;


int main(int argc, char** argv) {

    try {
        std::string device = "/dev/input/js0";
        if(argc > 1) {
            device = argv[1];
        }

        sys::joystick joystick(device);
        std::string name = joystick.get_name();
        int b = joystick.get_n_buttons();
        int a = joystick.get_n_axes();
        std::string amap = joystick.get_axes_map().substr(0, a);
        std::string bmap = joystick.get_button_map().substr(0, 2*b);
        sys::joystick::corrections corr = joystick.get_corrections();

        std::cout << device << " -> " << name << std::endl;
        std::cout << "buttons:  " << b << std::endl;
        std::cout << "axes:     " << a << std::endl;
        std::cout << "corrections: " << corr << std::endl;
        std::cout << "button map: " << util::hex_value(bmap) << std::endl;
        std::cout << "axes map: " << util::hex_value(amap) << std::endl;

        for(int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            std::cout << "executing arg " << i << ": " << arg << std::endl;

            if(arg == "--remap-axes" && (i + 1) < argc) {
                std::string arg2 = argv[++i];
                std::string namap(amap.size(), 0);
                std::vector<std::string> axes = util::tokenize(arg2);
                
                if(axes.size() > amap.size()) {
                    std::cout << "warning: axes list has " << axes.size() << " entries, but internal axes map has only " << amap.size() << std::endl;
                }

                for(unsigned int x = 0; x < axes.size() && x < amap.size(); x++) {
                    namap[x] = util::int_value(axes[x]);
                }

                std::string nnamap = joystick.set_axes_map(namap).substr(0, a);
                std::cout << "axes map: " << util::hex_value(nnamap) << std::endl;
            }

            if(arg == "--read-events") {
                while(true) {
                    sys::joystick::event e = joystick.read();
                    //if(!(e.jse.type & JS_EVENT_AXIS) || (std::abs(e.jse.value) > 16000))
                    std::cout << e << std::endl;
                } 
            }

        }
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    } catch(...) {
        std::cerr << "unknown exception" << std::endl;
        exit(1);
    }

    exit(0);
}

