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

#include <jlib/sys/joystick.hh>
#include <jlib/util/util.hh>
#include <bits/char_traits.h>

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

