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

#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace jlib {
namespace sys {

namespace {

/**
 * poll(2), built on every platform.
 *
 * The fallback, and the reference: when a native backend and this one disagree
 * about what a half-closed socket reports, this is the one whose behaviour is
 * written down in POSIX.  It is also what sys_reactor_test compares against,
 * which is why JLIB_SYS_REACTOR can select it on a machine that has kqueue.
 */
class poll_backend : public reactor_backend {
public:
    void add(int fd, reactor::event_type events, reactor::token t) {
        m_fds.push_back(pollfd_for(fd, events));
        m_tokens.push_back(t);
    }

    void modify(int fd, reactor::event_type, reactor::event_type events,
                reactor::token)
    {
        const std::size_t at = find(fd);

        if(at == m_fds.size()) return;

        m_fds[at].events = short(mask_of(events));
    }

    void remove(int fd, reactor::event_type) {
        const std::size_t at = find(fd);

        if(at == m_fds.size()) return;

        // Swap with the back rather than erase from the middle: the order of
        // the array is not part of the contract, and the reactor does not
        // promise an order within a pass either.
        m_fds[at] = m_fds.back();
        m_tokens[at] = m_tokens.back();

        m_fds.pop_back();
        m_tokens.pop_back();
    }

    std::size_t wait(std::vector<ready>& out, std::chrono::nanoseconds timeout) {
        if(m_fds.empty() && timeout < std::chrono::nanoseconds::zero()) {
            // Nothing to wait on and no deadline: poll(2) would block forever
            // on an empty set with -1, which is a hang rather than an answer.
            throw reactor::exception("waiting forever on nothing");
        }

        const int ms = millis(timeout);

        for(std::size_t i = 0; i < m_fds.size(); i++) m_fds[i].revents = 0;

        const int r = ::poll(m_fds.empty() ? 0 : &m_fds[0],
                             nfds_t(m_fds.size()), ms);

        if(r < 0) {
            if(errno == EINTR) return 0;

            throw reactor::exception(std::string("error in poll(): ") +
                                     std::strerror(errno));
        }

        if(r == 0) return 0;

        std::size_t found = 0;

        for(std::size_t i = 0; i < m_fds.size(); i++) {
            const reactor::event_type e = events_of(m_fds[i].revents);

            if(e == reactor::NONE) continue;

            out.push_back(ready(m_tokens[i], e));
            found++;
        }

        return found;
    }

    const char* name() const { return "poll"; }

private:
    static short mask_of(reactor::event_type events) {
        short m = 0;

        if(events & reactor::READ)  m = short(m | POLLIN);
        if(events & reactor::WRITE) m = short(m | POLLOUT);

        return m;
    }

    static reactor::event_type events_of(short revents) {
        reactor::event_type e = reactor::NONE;

        if(revents & POLLIN)  e |= reactor::READ;
        if(revents & POLLOUT) e |= reactor::WRITE;

        // Reported without being asked for, on every backend.  POLLNVAL is a
        // descriptor poll(2) does not recognise, which is a caller error
        // rather than a peer's, but there is nowhere better to put it than
        // beside the other thing a caller has to handle.
        if(revents & (POLLERR | POLLNVAL)) e |= reactor::ERROR;

        // Separate from ERROR, and separate from READ, because a peer that
        // sent data and closed gives POLLHUP with the data still waiting.
        if(revents & POLLHUP) e |= reactor::HANGUP;

        return e;
    }

    static pollfd pollfd_for(int fd, reactor::event_type events) {
        pollfd p;

        p.fd = fd;
        p.events = mask_of(events);
        p.revents = 0;

        return p;
    }

    static int millis(std::chrono::nanoseconds t) {
        if(t < std::chrono::nanoseconds::zero()) return -1;

        // Rounded **up**, so a timer can never be dispatched early by the
        // sub-millisecond remainder poll(2) cannot express.
        const std::chrono::milliseconds ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                t + std::chrono::nanoseconds(999999));

        return int(ms.count());
    }

    std::size_t find(int fd) const {
        for(std::size_t i = 0; i < m_fds.size(); i++)
            if(m_fds[i].fd == fd) return i;

        return m_fds.size();
    }

    std::vector<pollfd>          m_fds;
    std::vector<reactor::token>  m_tokens;
};

}

std::unique_ptr<reactor_backend> make_poll_backend() {
    return std::unique_ptr<reactor_backend>(new poll_backend);
}

}
}
