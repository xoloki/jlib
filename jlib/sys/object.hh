/* -*- mode: C++ c-basic-offset: 4 -*-
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

#ifndef JLIB_SYS_OBJECT_HH
#define JLIB_SYS_OBJECT_HH


namespace jlib {
namespace sys {

/**
 * A polymorphic base, so derived types can be deleted through a base pointer.
 *
 * This used to carry an intrusive reference count -- a plain int, incremented
 * and decremented without synchronization, whose unreference() did "delete
 * this" from a non-virtual member.  The count existed only to drive
 * Glib::RefPtr, which glibmm 2.68 reworked and which jlib no longer uses;
 * nothing ever called reference(), unreference() or refcount() directly.  The
 * owning smart pointer is now std::shared_ptr, which keeps the count outside
 * the object and manages it atomically.
 *
 * It also used to derive from sigc::trackable.  That base was vestigial here:
 * no slot bound to an Object was ever connected to a signal that outlived it.
 */
class Object {
public:
    Object() = default;
    virtual ~Object() = default;
};

}
}

#endif //JLIB_SYS_OBJECT_HH
