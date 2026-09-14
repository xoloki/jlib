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
#ifndef JLIB_SYS_ASYNC_WRITER_HH
#define JLIB_SYS_ASYNC_WRITER_HH

#include <jlib/sys/await.hh>
#include <jlib/sys/reactor.hh>
#include <jlib/sys/task.hh>

#include <cstddef>
#include <string>

namespace jlib {
namespace sys {

/**
 * Somewhere to put octets, that can wait for room without blocking a thread.
 *
 * The other half of async_reader.  One operation -- write all of this -- and
 * the same reason for being virtual: TLS answers "can I write" differently
 * from a descriptor, and a caller should not have to know which it has.
 *
 * ## Write-all, not write-some
 *
 * A partial write is the implementation's problem, not the caller's.  The
 * blocking side got this wrong for years: basic_socketbuf::sync() looped over
 * a short write and, on failure, left the put area untouched -- so a retry
 * resent the octets the kernel had already taken.  #204 fixed it by moving the
 * remainder rather than the pointer, and the shape here is what that bug
 * argues for: there is no partially-written state for a caller to mishandle,
 * because write() does not return until everything is gone or it throws.
 */
class async_writer {
public:
    virtual ~async_writer() {}

    async_writer(const async_writer&) = delete;
    async_writer& operator=(const async_writer&) = delete;

    /**
     * All of it, or throw.
     *
     * @throws cancelled if a token was set
     * @throws whatever the implementation throws when the peer has gone
     */
    virtual task<void> write(const char* data, std::size_t n) = 0;

    task<void> write(const std::string& s) {
        co_await write(s.data(), s.size());
    }

protected:
    async_writer() {}
};

/**
 * An async_writer over a descriptor.
 *
 * Does not set O_NONBLOCK, for the reason async_fd_reader gives: the reactor
 * reports writability and a write to a ready descriptor takes at least one
 * octet without blocking.  A *short* write is expected and looped over; a
 * blocking one is what the flag would prevent and what readiness already does.
 */
class async_fd_writer : public async_writer {
public:
    async_fd_writer(reactor& r, int fd, cancel_token t = cancel_token())
        : m_reactor(r), m_fd(fd), m_token(t) {}

    using async_writer::write;

    task<void> write(const char* data, std::size_t n);

private:
    reactor&     m_reactor;
    int          m_fd;
    cancel_token m_token;
};

}
}

#endif // JLIB_SYS_ASYNC_WRITER_HH
