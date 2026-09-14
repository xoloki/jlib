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
#ifndef JLIB_SYS_REACTOR_BACKEND_HH
#define JLIB_SYS_REACTOR_BACKEND_HH

#include <jlib/sys/reactor.hh>

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace jlib {
namespace sys {

/**
 * What a reactor waits with.  Not installed; see reactor.hh.
 */
class reactor_backend {
public:
    typedef std::pair<reactor::token, reactor::event_type> ready;

    virtual ~reactor_backend() {}

    virtual void add(int fd, reactor::event_type events, reactor::token t) = 0;

    virtual void modify(int fd, reactor::event_type was,
                        reactor::event_type events, reactor::token t) = 0;

    /**
     * @param events what was armed.
     *
     * The one asymmetry the portable surface cannot hide.  kqueue holds one
     * kevent per (ident, filter), so a READ|WRITE registration is two of them
     * and disarming needs to know which; epoll holds one struct per descriptor
     * and poll one pollfd, and neither cares.  Passing the mask costs those
     * two nothing and is the difference between a correct kqueue backend and
     * one that leaks a filter per connection.
     */
    virtual void remove(int fd, reactor::event_type events) = 0;

    /**
     * Wait, and append what is ready.
     *
     * @param timeout negative waits with no deadline, zero does not wait.
     * @return how many were appended.
     *
     * EINTR is the caller's to restart: this returns 0 and does not throw, so
     * a signal is neither an error nor a timeout.  Anything else throws
     * reactor::exception.
     */
    virtual std::size_t wait(std::vector<ready>& out,
                             std::chrono::nanoseconds timeout) = 0;

    virtual const char* name() const = 0;
};

/**
 * The native backend, unless JLIB_SYS_REACTOR names another.
 *
 * Read per construction rather than once, so a single test process can
 * exercise every backend the build has -- which is the only thing keeping the
 * poll fallback from rotting on a machine that has kqueue.
 */
std::unique_ptr<reactor_backend> make_reactor_backend();

std::unique_ptr<reactor_backend> make_poll_backend();

#ifdef HAVE_KQUEUE
std::unique_ptr<reactor_backend> make_kqueue_backend();
#endif

#ifdef HAVE_EPOLL
std::unique_ptr<reactor_backend> make_epoll_backend();
#endif

}
}

#endif // JLIB_SYS_REACTOR_BACKEND_HH
