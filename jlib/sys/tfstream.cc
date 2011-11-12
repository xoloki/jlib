/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2000 Joe Yandle <jwy@divisionbyzero.com>
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

