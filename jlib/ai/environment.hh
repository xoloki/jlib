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

#ifndef JLIB_AI_ENVIRONMENT_HH
#define JLIB_AI_ENVIRONMENT_HH


#include <jlib/ai/percept.hh>
#include <jlib/ai/action.hh>
#include <jlib/ai/agent.hh>


namespace jlib {
namespace ai {

class Environment {
public:
    typedef double score;

    Environment();
    virtual ~Environment();

    virtual Percept::ptr perceive(Agent::ptr agent, Percept::sense s) = 0;
    virtual Percept::map perceive(Agent::ptr agent) = 0;

    virtual void act(Action::ptr a) = 0;

    virtual score run();
    virtual score run(int x);

    virtual score run(Agent::ptr agent) = 0;

private:
    Agent::list mAgents;
};


}
}

#endif //JLIB_AI_ENVIRONMENT_HH
