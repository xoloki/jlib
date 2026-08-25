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

#ifndef JLIB_AI_PERCEPT_HH
#define JLIB_AI_PERCEPT_HH


#include <jlib/sys/object.hh>
#include <memory>

#include <list>
#include <map>


namespace jlib {
namespace ai {

class Percept : public sys::Object {
public:
    typedef int sense;
    typedef std::shared_ptr<Percept> ptr;
    typedef std::list<ptr> list;
    typedef std::map<sense, Percept::ptr> map;

    Percept();
    virtual ~Percept();

private:
};


}
}

#endif //JLIB_AI_PERCEPT_HH
