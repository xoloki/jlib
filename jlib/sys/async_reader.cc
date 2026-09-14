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
#include <jlib/sys/async_reader.hh>

#include <unistd.h>

#include <cerrno>

namespace jlib {
namespace sys {

task<bool> async_reader::fill() {
    if(m_closed) co_return false;

    for(;;) {
        // Throws cancelled if the token was set, which is how a framing
        // function inherits cancellation without knowing about it.
        co_await until_ready(m_reactor, m_fd, reactor::READ, m_token);

        const ssize_t n = ::read(m_fd, m_buf, sizeof m_buf);

        if(n > 0) {
            m_at = 0;
            m_end = static_cast<std::size_t>(n);

            co_return true;
        }

        if(n == 0) {
            m_closed = true;

            co_return false;
        }

        // Interrupted before anything arrived: go round rather than report a
        // close.  The streambuf layer reports EINTR as end of stream and
        // leaves the caller to ask afterwards, which nothing does; there is no
        // reason to carry that here.
        if(errno == EINTR) continue;

        // EAGAIN on a descriptor the reactor just called readable means a
        // spurious wakeup -- rare, and the answer is the same: wait again.
        if(errno == EAGAIN || errno == EWOULDBLOCK) continue;

        m_closed = true;

        co_return false;
    }
}

}
}
