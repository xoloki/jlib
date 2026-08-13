/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2000 Joe Yandle <jwy@divisionbyzero.com>
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
