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

#ifndef JLIB_AI_AGENT_HH
#define JLIB_AI_AGENT_HH


#include <jlib/ai/action.hh>
#include <jlib/ai/percept.hh>

#include <jlib/sys/object.hh>

#include <memory>


namespace jlib {
namespace ai {

class Agent : public sys::Object {
public:
    typedef std::shared_ptr<Agent> ptr;
    typedef std::list<ptr> list;

    Agent();
    virtual ~Agent();

    virtual Action::ptr agent(const Percept::map percepts) = 0;

private:
};


}
}

#endif //JLIB_AI_AGENT_HH
