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

#include <cstdlib>
#include <cstring>

namespace jlib {
namespace sys {

std::unique_ptr<reactor_backend> make_reactor_backend() {
    // Per construction, not once.  One test process can then exercise every
    // backend the build has, which is the only thing that keeps the poll
    // fallback from rotting on a machine that has kqueue.
    const char* want = std::getenv("JLIB_SYS_REACTOR");

    if(want != 0 && *want != 0) {
        if(std::strcmp(want, "poll") == 0) return make_poll_backend();

#ifdef HAVE_KQUEUE
        if(std::strcmp(want, "kqueue") == 0) return make_kqueue_backend();
#endif

#ifdef HAVE_EPOLL
        if(std::strcmp(want, "epoll") == 0) return make_epoll_backend();
#endif

        throw reactor::exception(std::string("JLIB_SYS_REACTOR asks for \"") +
                                 want + "\", which this build does not have");
    }

#ifdef HAVE_KQUEUE
    return make_kqueue_backend();
#elif defined(HAVE_EPOLL)
    return make_epoll_backend();
#else
    return make_poll_backend();
#endif
}

}
}
