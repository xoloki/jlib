/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 2010 Joe Yandle <joey@divisionbyzero.com>
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


#include <jlib/ai/environment.hh>


namespace jlib {
namespace ai {

Environment::Environment() 
{

}

Environment::~Environment() {
}

Environment::score Environment::run() {
    Environment::score s = 0;

    for(Agent::list::iterator i = mAgents.begin(); i != mAgents.end(); i++) {
        Agent::ptr agent = *i;
        s = run(agent);
    }
    
    return s;
}

Environment::score Environment::run(int x) {
    Environment::score s = 0;

    for(int i = 0; i < x; i++) {
        s += run();
    }

    return s/x;
}


}
}
