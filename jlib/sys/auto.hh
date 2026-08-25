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

#ifndef JLIB_SYS_AUTO_HH
#define JLIB_SYS_AUTO_HH

namespace jlib {
    namespace sys {

        template<class T>
        class auto_lock {
        public:
            auto_lock(T& t) : m_t(t) { m_t.lock(); }
            ~auto_lock() { m_t.unlock(); }
        private:
            T& m_t;
        };

    }
}
#endif //JLIB_SYS_AUTO_HH
