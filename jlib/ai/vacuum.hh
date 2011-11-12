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

#ifndef JLIB_AI_VACUUM_HH
#define JLIB_AI_VACUUM_HH


#include <jlib/ai/environment.hh>

#include <vector>


namespace jlib {
namespace ai {
namespace vacuum {

typedef std::vector<double> location;
    
class Move : public Action {
public:
    typedef Glib::RefPtr<Move> ptr;

    Move(location l) : loc(l) {}
    location loc;
};

class Clean : public Action {
public:
    typedef Glib::RefPtr<Clean> ptr;
    Clean() {}
};

class Location : public Percept {
public:
    Location(location l) : loc(l) {}
    location loc;
};

class Cleanliness : public Percept {
public:
    Cleanliness(bool b) : clean(b) {}
    bool clean;
};

class Agent : public ai::Agent {
public:

};

class Environment : public ai::Environment {
public:
    static const Percept::sense LOCATION = 0;
    static const Percept::sense CLEANLINESS = 1;

    Environment();
    virtual ~Environment() {}

    virtual Percept::ptr perceive(Agent::ptr agent, Percept::sense s);
    virtual Percept::map perceive(Agent::ptr agent);

    virtual void act(Agent::ptr agent, Action::ptr a);
private:
    std::map<Agent::ptr, location> mLocations;
    std::map<location, bool> mClean;
};


}
}
}

#endif //JLIB_AI_VACUUM_HH
