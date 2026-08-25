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
#include <fstream>
#include <sstream>
#include <cstdlib>

#include <jlib/sys/sys.hh>
#include <jlib/util/util.hh>

using namespace jlib;

int main(int argc, char** argv) {
    try {
	if(argc < 2) {
	    std::cout << argv[0] << " file.m3u [...]" << std::endl;
	    return 1;
	}

        std::string player = "mplayer";
	if(::getenv("JM3U_PLAYER")) {
	    player = ::getenv("JM3U_PLAYER");
	}

	for(int i = 1; i < argc; i++) {
	    std::string path = argv[i];
	    std::ifstream ifs(path.c_str());
	    while(ifs) {
		std::string mp3;
		sys::getline(ifs, mp3);
		if(ifs) {
		    mp3 = util::trim(mp3);
		    std::cout << "Playing " << mp3 << std::endl;
		    std::stringstream ss;
		    ss << player << " \"" << mp3 << "\"";
		    std::string cmd = ss.str();
		    std::cout << cmd << std::endl;
		    sys::shell(cmd);
		}
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

