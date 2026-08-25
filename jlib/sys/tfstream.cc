/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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

#include <jlib/sys/tfstream.hh>

#include <sstream>

#include <cstdlib>
#include <cstring>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

const std::string BASE_PATH = "/tmp";
const mode_t g_mode = 0700;
const int MAX_TRY = 666;

namespace jlib {
    namespace sys {

        tfstream::tfstream() : std::fstream()// m_file.c_str(),std::ios_base::in | std::ios_base::out | std::ios_base::trunc )
        {
            bool bunny = true;
            int i=0;
            std::string base = BASE_PATH, 
                pre = "/.jlib";
	    std::ostringstream os;

            if(std::getenv("TMPDIR")) {
                base = std::getenv("TMPDIR");
            }
            while(bunny) {
		os.clear(); os << rand();
                m_dir = base + pre + os.str();
                if(!mkdir(m_dir.c_str(), g_mode)) {
                    bunny = false;
		    os.clear(); os << rand();
                    m_file = m_dir + "/" + os.str();
                }
                if(i++ > MAX_TRY) throw exception("couldn't create tmp file, too many tries");
            }
            open(m_file.c_str(),std::ios_base::in | std::ios_base::out | std::ios_base::trunc );
        }

        tfstream::~tfstream() {
            unlink(m_file.c_str());
            rmdir(m_dir.c_str());
        }

        long tfstream::size() {
            struct stat mystat;
            stat(m_file.c_str(), &mystat);
            return mystat.st_size;
        }

        stfstream::~stfstream() {
            unsigned int sz = size();
            if(sz > 0) {
                char* buf = new char[sz];
                for(unsigned int i=0;i<10;i++) {
                    std::memset(buf,i,sz);
                    this->seekp(0,std::ios_base::beg);
                    this->write(buf,sz);
                }
                delete [] buf;
            }
        }
        
    }
}

