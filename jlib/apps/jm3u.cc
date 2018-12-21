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

#include <jlib/sys/sys.hh>
#include <jlib/util/util.hh>

using namespace jlib;

int main(int argc, char** argv) {
    try {
	if(argc < 2) {
	    std::cout << argv[0] << " file.m3u" << std::endl;
	    return 1;
	}
	
        std::string path = argv[1];
	std::ifstream ifs(path.c_str());
	std::string mp3;
	while(ifs) {
	    sys::getline(ifs, mp3);
	    if(ifs) {
		mp3 = util::trim(mp3);
		std::cout << "Playing " << mp3 << std::endl;
		std::stringstream ss;
		ss << "mplayer \"" << mp3 << "\"";
		std::string cmd = ss.str();
		std::cout << cmd << std::endl;
		sys::shell(cmd);
	    }
	}

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

