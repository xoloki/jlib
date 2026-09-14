/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
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
#include <jlib/sys/reactor_backend.hh>

#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace jlib {
namespace sys {

namespace {

/**
 * kqueue, on macOS and the BSDs.
 *
 * Level-triggered, deliberately: no EV_CLEAR anywhere.  See reactor.hh for why
 * that is a contract rather than a default.
 */
class kqueue_backend : public reactor_backend {
public:
    kqueue_backend() {
        m_kq = ::kqueue();

        if(m_kq < 0) {
            throw reactor::exception(std::string("error in kqueue(): ") +
                                     std::strerror(errno));
        }

        // kqueue() has no CLOEXEC flag of its own, so this is the only way to
        // keep the descriptor out of every child sys::run() starts.
        const int flags = ::fcntl(m_kq, F_GETFD, 0);

        if(flags != -1) ::fcntl(m_kq, F_SETFD, flags | FD_CLOEXEC);
    }

    ~kqueue_backend() { if(m_kq >= 0) ::close(m_kq); }

    void add(int fd, reactor::event_type events, reactor::token t) {
        arm(fd, events, t);
    }

    void modify(int fd, reactor::event_type was, reactor::event_type events,
                reactor::token t)
    {
        // Disarm what is no longer wanted before arming what is: kqueue holds
        // one kevent per (ident, filter), so a filter dropped from the mask
        // stays armed unless it is deleted by name.
        disarm(fd, was & ~events);
        arm(fd, events & ~was, t);
    }

    void remove(int fd, reactor::event_type events) { disarm(fd, events); }

    std::size_t wait(std::vector<ready>& out, std::chrono::nanoseconds timeout) {
        struct timespec ts;
        struct timespec* tp = 0;

        if(timeout >= std::chrono::nanoseconds::zero()) {
            ts.tv_sec = time_t(timeout.count() / 1000000000);
            ts.tv_nsec = long(timeout.count() % 1000000000);
            tp = &ts;
        }

        m_events.resize(m_armed == 0 ? 1 : m_armed);

        const int n = ::kevent(m_kq, 0, 0, &m_events[0], int(m_events.size()), tp);

        if(n < 0) {
            if(errno == EINTR) return 0;

            throw reactor::exception(std::string("error in kevent(): ") +
                                     std::strerror(errno));
        }

        std::size_t found = 0;

        for(int i = 0; i < n; i++) {
            const struct kevent& k = m_events[std::size_t(i)];

            reactor::event_type e = reactor::NONE;

            if(k.filter == EVFILT_READ)  e |= reactor::READ;
            if(k.filter == EVFILT_WRITE) e |= reactor::WRITE;

            // EV_ERROR with data is a failed change, not a ready descriptor.
            if(k.flags & EV_ERROR) {
                if(k.data != 0) e |= reactor::ERROR;
            }

            // Separate from READ, and delivered with it: a peer that sent data
            // and closed reports EV_EOF with the data still waiting.
            if(k.flags & EV_EOF) e |= reactor::HANGUP;

            if(e == reactor::NONE) continue;

            out.push_back(ready(reactor::token(std::uintptr_t(k.udata)), e));
            found++;
        }

        return found;
    }

    const char* name() const { return "kqueue"; }

private:
    void change(int fd, short filter, u_short flags, reactor::token t) {
        struct kevent k;

        EV_SET(&k, uintptr_t(fd), filter, flags, 0, 0,
               reinterpret_cast<void*>(std::uintptr_t(t)));

        if(::kevent(m_kq, &k, 1, 0, 0, 0) < 0) {
            // ENOENT deleting something already gone is benign, and is what a
            // close() before remove() leaves behind.
            if((flags & EV_DELETE) && errno == ENOENT) return;

            throw reactor::exception(std::string("error in kevent(): ") +
                                     std::strerror(errno));
        }
    }

    void arm(int fd, reactor::event_type events, reactor::token t) {
        if(events & reactor::READ) {
            change(fd, EVFILT_READ, EV_ADD | EV_ENABLE, t);
            m_armed++;
        }

        if(events & reactor::WRITE) {
            change(fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, t);
            m_armed++;
        }
    }

    void disarm(int fd, reactor::event_type events) {
        if(events & reactor::READ) {
            change(fd, EVFILT_READ, EV_DELETE, reactor::token::none);
            if(m_armed) m_armed--;
        }

        if(events & reactor::WRITE) {
            change(fd, EVFILT_WRITE, EV_DELETE, reactor::token::none);
            if(m_armed) m_armed--;
        }
    }

    int m_kq = -1;

    // Filters, not descriptors: a READ|WRITE registration is two of them, and
    // the event array has to be big enough for both to fire at once.
    std::size_t m_armed = 0;

    std::vector<struct kevent> m_events;
};

}

std::unique_ptr<reactor_backend> make_kqueue_backend() {
    return std::unique_ptr<reactor_backend>(new kqueue_backend);
}

}
}
