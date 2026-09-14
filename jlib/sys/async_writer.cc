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
#include <jlib/sys/async_writer.hh>
#include <jlib/sys/sys.hh>

#include <unistd.h>

#include <cerrno>
#include <stdexcept>

namespace jlib {
namespace sys {

task<void> async_fd_writer::write(const char* data, std::size_t n) {
    std::size_t sent = 0;

    while(sent < n) {
        // Blocked here rather than around the whole loop: a sigpipe_guard is
        // per-thread and the thread may run another coroutine at every
        // suspension, so holding one across a co_await would block SIGPIPE for
        // whatever ran next.
        ssize_t took;

        {
            sigpipe_guard guard;

            took = ::write(m_fd, data + sent, n - sent);
        }

        if(took > 0) {
            sent += static_cast<std::size_t>(took);

            continue;
        }

        if(took < 0 && errno == EINTR) continue;

        if(took < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // The socket buffer is full.  Wait for room rather than spin.
            co_await until_ready(m_reactor, m_fd, reactor::WRITE, m_token);

            continue;
        }

        // EPIPE arrives here as an error rather than as a signal, which is
        // what the guard above buys.
        throw std::runtime_error("the peer closed while " +
                                 std::to_string(n - sent) +
                                 " octets were still to be written");
    }

    co_return;
}

}
}
