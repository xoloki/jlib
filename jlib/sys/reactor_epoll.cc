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

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace jlib {
namespace sys {

namespace {

/**
 * epoll, on Linux.
 *
 * Level-triggered, deliberately: no EPOLLET.  See reactor.hh.
 *
 * EPOLLRDHUP is **not** requested.  A half-close that still has data waiting
 * is exactly the case HANGUP must not swallow, and the simplest way to get
 * that right is not to ask the question.
 */
class epoll_backend : public reactor_backend {
public:
    epoll_backend() {
        m_ep = ::epoll_create1(EPOLL_CLOEXEC);

        if(m_ep < 0) {
            throw reactor::exception(std::string("error in epoll_create1(): ") +
                                     std::strerror(errno));
        }
    }

    ~epoll_backend() { if(m_ep >= 0) ::close(m_ep); }

    void add(int fd, reactor::event_type events, reactor::token t) {
        struct epoll_event e = event_for(events, t);

        if(::epoll_ctl(m_ep, EPOLL_CTL_ADD, fd, &e) < 0) {
            throw reactor::exception(std::string("error in epoll_ctl(ADD): ") +
                                     std::strerror(errno));
        }

        m_count++;
    }

    void modify(int fd, reactor::event_type, reactor::event_type events,
                reactor::token t)
    {
        struct epoll_event e = event_for(events, t);

        if(::epoll_ctl(m_ep, EPOLL_CTL_MOD, fd, &e) < 0) {
            throw reactor::exception(std::string("error in epoll_ctl(MOD): ") +
                                     std::strerror(errno));
        }
    }

    void remove(int fd, reactor::event_type) {
        if(::epoll_ctl(m_ep, EPOLL_CTL_DEL, fd, 0) < 0) {
            // ENOENT and EBADF are what a descriptor closed before it was
            // removed leaves behind, and closing drops it from the set
            // anyway.
            if(errno == ENOENT || errno == EBADF) { if(m_count) m_count--; return; }

            throw reactor::exception(std::string("error in epoll_ctl(DEL): ") +
                                     std::strerror(errno));
        }

        if(m_count) m_count--;
    }

    std::size_t wait(std::vector<ready>& out, std::chrono::nanoseconds timeout) {
        int ms = -1;

        if(timeout >= std::chrono::nanoseconds::zero()) {
            // Rounded up, so a timer cannot fire early on the sub-millisecond
            // remainder epoll_wait cannot express.
            ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                         timeout + std::chrono::nanoseconds(999999)).count());
        }

        m_events.resize(m_count == 0 ? 1 : m_count);

        const int n = ::epoll_wait(m_ep, &m_events[0], int(m_events.size()), ms);

        if(n < 0) {
            if(errno == EINTR) return 0;

            throw reactor::exception(std::string("error in epoll_wait(): ") +
                                     std::strerror(errno));
        }

        std::size_t found = 0;

        for(int i = 0; i < n; i++) {
            const struct epoll_event& k = m_events[std::size_t(i)];

            reactor::event_type e = reactor::NONE;

            if(k.events & EPOLLIN)  e |= reactor::READ;
            if(k.events & EPOLLOUT) e |= reactor::WRITE;
            if(k.events & EPOLLERR) e |= reactor::ERROR;
            if(k.events & EPOLLHUP) e |= reactor::HANGUP;

            if(e == reactor::NONE) continue;

            out.push_back(ready(reactor::token(k.data.u64), e));
            found++;
        }

        return found;
    }

    const char* name() const { return "epoll"; }

private:
    static struct epoll_event event_for(reactor::event_type events,
                                        reactor::token t)
    {
        struct epoll_event e;

        std::memset(&e, 0, sizeof e);

        if(events & reactor::READ)  e.events |= EPOLLIN;
        if(events & reactor::WRITE) e.events |= EPOLLOUT;

        e.data.u64 = std::uint64_t(t);

        return e;
    }

    int m_ep = -1;
    std::size_t m_count = 0;

    std::vector<struct epoll_event> m_events;
};

}

std::unique_ptr<reactor_backend> make_epoll_backend() {
    return std::unique_ptr<reactor_backend>(new epoll_backend);
}

}
}
