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

#ifndef JLIB_AI_VACUUM_HH
#define JLIB_AI_VACUUM_HH


#include <jlib/ai/environment.hh>

#include <memory>
#include <vector>


namespace jlib {
namespace ai {
namespace vacuum {

typedef std::vector<double> location;
    
class Move : public Action {
public:
    typedef std::shared_ptr<Move> ptr;

    Move(location l) : loc(l) {}
    location loc;
};

class Clean : public Action {
public:
    typedef std::shared_ptr<Clean> ptr;
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
    // constexpr, not const.  A `static const` with an in-class initialiser is
    // only a declaration: using one where an lvalue is required -- ret[LOCATION]
    // in vacuum.cc does -- odr-uses it and wants a definition that was never
    // written.  constexpr is implicitly inline in C++17 and supplies one.
    //
    // Latent since this was written and found only now, because nothing had
    // ever linked libjai: on Linux the shared object carried undefined
    // references to both, and the first executable to link it failed.
    static constexpr Percept::sense LOCATION = 0;
    static constexpr Percept::sense CLEANLINESS = 1;

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
