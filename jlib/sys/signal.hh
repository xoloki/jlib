/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_SYS_SIGNAL_HH
#define JLIB_SYS_SIGNAL_HH

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace jlib {
namespace sys {

/**
 * a slot is just a callable; this alias exists so call sites read the same
 * way they did under libsigc++.
 */
template<typename Signature>
using slot = std::function<Signature>;

/**
 * a handle to one signal/slot connection.
 *
 * libsigc++ solved the dangling-slot problem from the receiver side, by
 * having receivers derive from sigc::trackable so their destructor could
 * disconnect them.  Nothing in jlib actually relied on that (every one of
 * the six trackable bases was vestigial), so this takes the simpler and more
 * explicit route: the caller keeps a connection and disconnects when it means
 * to.  The connection holds a weak reference, so it stays safe to call
 * disconnect() after the signal itself is gone.
 */
class connection {
public:
    connection() = default;

    explicit connection(std::function<void()> disconnector)
        : m_disconnect(std::move(disconnector)) {}

    void disconnect() {
        if(m_disconnect) {
            m_disconnect();
            m_disconnect = nullptr;
        }
    }

    bool connected() const { return static_cast<bool>(m_disconnect); }

private:
    std::function<void()> m_disconnect;
};

template<typename Signature>
class signal;

/**
 * a list of callables invoked together.
 *
 * Spelled with the function-signature syntax, i.e. signal<void(int,int)>,
 * matching libsigc++-3 rather than the numbered signalN templates of
 * libsigc++-2 that this replaced.
 *
 * NOTE: always name this qualified, as sys::signal<...>.  An unqualified
 * signal is ambiguous against POSIX ::signal(int, void(*)(int)) from
 * <signal.h>, so do not add a "using namespace jlib::sys" to a translation
 * unit that also pulls in the C signal API.
 */
template<typename R, typename... Args>
class signal<R(Args...)> {
public:
    typedef std::function<R(Args...)> slot_type;
    typedef std::size_t id_type;

    signal() : m_impl(std::make_shared<impl>()) {}

    connection connect(slot_type fn) {
        const id_type id = m_impl->next++;
        m_impl->slots.push_back(entry{id, std::move(fn)});

        std::weak_ptr<impl> weak = m_impl;
        return connection([weak, id]() {
            if(std::shared_ptr<impl> p = weak.lock()) {
                p->slots.erase(std::remove_if(p->slots.begin(), p->slots.end(),
                                              [id](const entry& e) { return e.id == id; }),
                               p->slots.end());
            }
        });
    }

    void clear() { m_impl->slots.clear(); }

    std::size_t size() const { return m_impl->slots.size(); }
    bool empty() const { return m_impl->slots.empty(); }

    /**
     * Slots are copied before being run so that a slot may connect or
     * disconnect during emission without invalidating the iteration.  For a
     * non-void signal the last slot's value is returned, as libsigc++ did.
     */
    R emit(Args... args) const {
        std::vector<entry> slots = m_impl->slots;

        if constexpr(std::is_void_v<R>) {
            for(const entry& e : slots) {
                e.fn(args...);
            }
        } else {
            R ret{};
            for(const entry& e : slots) {
                ret = e.fn(args...);
            }
            return ret;
        }
    }

    R operator()(Args... args) const { return emit(args...); }

private:
    struct entry {
        id_type id;
        slot_type fn;
    };

    struct impl {
        std::vector<entry> slots;
        id_type next = 0;
    };

    std::shared_ptr<impl> m_impl;
};

}
}

#endif // JLIB_SYS_SIGNAL_HH
