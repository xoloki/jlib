/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2010 Joey Yandle <xoloki@gmail.com>
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
    Move::ptr move = std::dynamic_pointer_cast<Move>(a);
    Clean::ptr clean = std::dynamic_pointer_cast<Clean>(a);

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
