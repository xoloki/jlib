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


#include <jlib/ai/vacuum.hh>


namespace jlib {
namespace ai {
namespace vacuum {

Environment::Environment() 
{
    
}

Percept::ptr Environment::perceive(Agent::ptr agent, Percept::sense s) {
    Percept::ptr ret;

    switch(s) {
    case LOCATION:
        ret = Percept::ptr(new Location(mLocations[agent]));
        break;
    case CLEANLINESS:
        ret = Percept::ptr(new Cleanliness(mClean[mLocations[agent]]));
        break;
    }

    return ret;
}

Percept::map Environment::perceive(Agent::ptr agent) {
    Percept::map ret;

    ret[LOCATION] = perceive(agent, LOCATION);
    ret[CLEANLINESS] = perceive(agent, CLEANLINESS);

    return ret;
}

void Environment::act(Agent::ptr agent, Action::ptr a) {
    Move::ptr move = Move::ptr::cast_dynamic(a);
    Clean::ptr clean = Clean::ptr::cast_dynamic(a);

    if(move) {
        mLocations[agent] = move->loc;
    } else if(clean) {
        location l = mLocations[agent];
        mClean[l] = true;
    }
}


}
}
}
