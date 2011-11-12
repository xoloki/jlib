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
#include <fstream>
#include <map>
#include <set>

#include <cmath>
#include <cstdlib>

#include <unistd.h>

#include <jlib/x/Display.hh>

#include <jlib/sys/sys.hh>
#include <jlib/sys/joystick.hh>

#include <jlib/util/util.hh>

#include <bits/char_traits.h>

const bool DEBUG = getenv("JJOY2XEV_DEBUG");

using namespace jlib;

unsigned int MAX = 32767;
unsigned int DEAD = 16000;

x::Display display;

typedef std::map<int, std::pair<unsigned int, unsigned int> > axis_map;
typedef std::map<int, unsigned int> button_map;
typedef std::set<unsigned int> axis_set;

axis_map amap;
axis_set trigger;
axis_set dpad;

void init_axis_map(std::istream& in) {
    while(in) {
        std::string s;
        sys::getline(in, s);
        std::vector<std::string> t = util::tokenize(s, " ", false);

        if(t.size() < 2) {
            if(s.length())
                std::cerr << "init_axis_map: t.size() = " << t.size() << " for line: " << s << std::endl;
            return;
        }            

        unsigned int i = util::int_value(t[0]);
        KeyCode code = display.code(t[1]);

        switch(t.size()) {
        case 4:
            if(t[3] == "T")
                trigger.insert(i);
            else if(t[3] == "D") 
                dpad.insert(i);
            
        case 3:
            amap[i] = std::make_pair(code, display.code(t[2]));
            break;
        case 2:
            amap[i] = std::make_pair(code, code);
            break;
        }
    }
}

button_map bmap;

void init_button_map(std::istream& in) {
    while(in) {
        std::string s;
        sys::getline(in, s);
        std::vector<std::string> t = util::tokenize(s, " ", false);

        if(t.size() < 2) {
            if(s.length())
                std::cerr << "init_button_map: t.size() = " << t.size() << " for line: " << s << std::endl;
            return;
        }            

        int i = util::int_value(t[0]);
        KeyCode code = display.code(t[1]);

        bmap[i] = code;
    }
}

bool is_dead(int value) {
    return std::abs(static_cast<double>(value)) < DEAD;
}

bool is_trigger(unsigned int axis) {
    return trigger.find(axis) != trigger.end();
}

bool is_dpad(unsigned int axis) {
    return dpad.find(axis) != dpad.end();
}

void send_event(const XEvent& e) {
    if(DEBUG)
        std::cout << (e.type == KeyPress ? "Press" : "Release") << " keycode " << e.xkey.keycode << std::endl;
    display.send_event(e);
}


void send_key_event(int type, unsigned int code) {
    XEvent xev;

    xev.type = type;
    xev.xkey.state = 0;
    xev.xkey.keycode = code;

    send_event(xev);
}


void handle_trigger(sys::joystick::event e, int current) {
    //bool pressed = e.jse.value == MAX;
    bool pressed = (e.jse.value > 0), was_pressed = (current > 0);
    
    if(pressed != was_pressed)
        send_key_event(pressed ? KeyPress : KeyRelease, amap[e.jse.number].first);
}

void handle_dpad(sys::joystick::event e, int current) {
    bool pressed = std::abs(static_cast<double>(e.jse.value)) == MAX;
    bool positive = e.jse.value == MAX;
    int n = e.jse.number;
    int v = e.jse.value;

    XEvent xev;
    xev.type = pressed ? KeyPress : KeyRelease;
    xev.xkey.state = 0;

    //case 6: // D-pad, only gives +/- 32K, 0
    //case 7: // D-pad, only gives +/- 32K, 0
    unsigned int pcode = (e.jse.number == 6 ? 10 : 13);
    unsigned int ncode = (e.jse.number == 6 ? 12 : 11);

    if(pressed)
        xev.xkey.keycode = positive ? pcode : ncode;
    else 
        xev.xkey.keycode = current == MAX ? pcode : ncode;

    send_event(xev);
}    
    
//xev.xkey.keycode = pcode;
//std::cout << "Release keycode " << xev.xkey.keycode << std::endl;
//display.send_event(xev);

//xev.xkey.keycode = ncode;
//std::cout << "Release keycode " << xev.xkey.keycode << std::endl;
//display.send_event(xev);


void handle_axis(sys::joystick::event e) {
    static std::map<int, int> current;

    int n = e.jse.number;
    int v = e.jse.value;
    bool pressed = !is_dead(v);
    std::map<int,int>::iterator i = current.find(n);

    //if(!is_trigger(n) && i != current.end() && is_dead(i->second) == is_dead(v) && pressed)
    if(!is_trigger(n) && i != current.end() && is_dead(i->second) == is_dead(v))
    //if(i != current.end() && is_dead(i->second) == is_dead(v))
        return;

    int c = i != current.end() ? i->second : 0;
    current[n] = v; 

    if(is_trigger(n))
        return handle_trigger(e, c);
    
    if(is_dpad(n))
        return handle_dpad(e, c);

    bool pos = e.jse.value > 0;
    bool zero = e.jse.value == 0;

    if(pressed) {
        send_key_event(KeyPress, !pos ? amap[n].first : amap[n].second);
    } else {
        send_key_event(KeyRelease, amap[n].first);
        send_key_event(KeyRelease, amap[n].second);
    }
}


void handle_button(sys::joystick::event e) {
    bool pressed = e.jse.value;
    XEvent xev;

    xev.type = pressed ? KeyPress : KeyRelease;
    xev.xkey.state = 0;
    xev.xkey.keycode = bmap[e.jse.number];
    
    send_event(xev);
}

int main(int argc, char** argv) {
    try {
        std::string device = "/dev/input/js0";
        if(argc > 1)
            device = argv[1];

        std::string apath = "/home/xoloki/src/jlib-1.2/jlib/apps/wow-axes.map";
        if(argc > 2) 
            apath = argv[2];

        std::string bpath = "/home/xoloki/src/jlib-1.2/jlib/apps/wow-buttons.map";
        if(argc > 3) 
            bpath = argv[3];

        sys::joystick joystick(device);
        std::ifstream axes(apath.data());
        std::ifstream buttons(bpath.data());

        init_axis_map(axes);
        init_button_map(buttons);

        while(true) {
            sys::joystick::event e = joystick.read();
            if(DEBUG)
                std::cout << e << std::endl;

            if(e.jse.type & JS_EVENT_AXIS)
                handle_axis(e);
            else
                handle_button(e);
        } 
    }
    catch(std::exception& e) {
        std::cerr << e.what() << std::endl;
        std::exit(1);
    } catch(...) {
        std::cerr << "unknown exception" << std::endl;
        std::exit(1);
    }

    std::exit(0);
}

