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
#ifndef JLIB_SYS_ASYNC_READER_HH
#define JLIB_SYS_ASYNC_READER_HH

#include <jlib/sys/await.hh>
#include <jlib/sys/reactor.hh>
#include <jlib/sys/task.hh>

#include <cstddef>
#include <cstring>
#include <string>

namespace jlib {
namespace sys {

/**
 * A buffer over a descriptor that can be topped up without blocking a thread.
 *
 * What an async framing function reads from.  `get()` hands back one octet
 * from the buffer; when the buffer is dry it says so, and `fill()` is awaited
 * to get more.
 *
 * ## get() is synchronous and fill() is the only coroutine
 *
 * Which is the whole shape, and it answers the obvious objection.  A
 * `co_await in.get()` per octet would allocate a coroutine frame per octet;
 * here a framing function is **one** frame and suspends only when the buffer
 * is actually empty.  Reading a message head delivered in three pieces cost
 * thirteen heap allocations end to end, counted rather than estimated.
 *
 * ## The descriptor stays blocking, and that is deliberate
 *
 * Nothing here sets O_NONBLOCK.  The reactor reports a descriptor readable and
 * a read on a ready descriptor returns without blocking, so the flag is not
 * needed -- and leaving it alone is what keeps listener.cc's unconditional
 * strip intact.  That line exists because BSD and macOS *inherit* the flag on
 * an accepted descriptor and Linux does not, and its comment says the
 * divergence is "the worst way for a difference like this to be found".
 *
 * The cost is that a spurious readiness -- rare on a stream socket, possible
 * in principle -- blocks the reactor's thread in read() rather than returning
 * EAGAIN.  Worth knowing; not worth the flag until something shows it
 * happening.
 *
 * ## What it does not do
 *
 * No TLS.  basic_tlsbuf shares basic_socketbuf's descriptor and buffers by
 * inheritance and answers a *second* readiness question -- whether the SSL has
 * buffered plaintext -- that a descriptor cannot.  #4 records that as the real
 * work behind the issue and not something a reader wraps its way out of.
 *
 * No writing, no seeking, no putback.  It is a read buffer, and it is the
 * least that read_head needs.
 */
class async_reader {
public:
    /** What get() answers when the buffer is empty.  Not a valid octet. */
    static const int empty = -2;

    /**
     * @param fd  not taken over: this neither closes it nor changes its flags
     * @param t   cancels a fill() that is waiting; see cancel_token
     */
    async_reader(reactor& r, int fd, cancel_token t = cancel_token())
        : m_reactor(r), m_fd(fd), m_token(t) {}

    async_reader(const async_reader&) = delete;
    async_reader& operator=(const async_reader&) = delete;

    /** One octet, or `empty`.  Never blocks, never suspends, never allocates. */
    int get() {
        if(m_at == m_end) return empty;

        return static_cast<unsigned char>(m_buf[m_at++]);
    }

    /**
     * Up to n octets from the buffer into out.
     *
     * @return how many were taken, which is zero when the buffer is dry.
     *
     * For a caller that knows how much it wants -- a body of a stated length,
     * a chunk.  get() in a loop would be correct and would cost a call per
     * octet; a megabyte body is a megabyte of them.
     *
     * Never blocks, never suspends, never allocates.
     */
    std::size_t take(char* out, std::size_t n) {
        const std::size_t have = m_end - m_at;
        const std::size_t took = n < have ? n : have;

        if(took == 0) return 0;

        std::memcpy(out, m_buf + m_at, took);

        m_at += took;

        return took;
    }

    /** How much is in hand without waiting. */
    std::size_t buffered() const { return m_end - m_at; }

    /** Whether the peer has closed and the buffer is spent. */
    bool spent() const { return m_closed && m_at == m_end; }

    /**
     * Wait for the descriptor, then read once.
     *
     * The only suspension point in any framing function built on this.
     *
     * @return false at end of stream, so a caller can tell "nothing more is
     *         coming" from "nothing yet" -- which is the distinction the
     *         synchronous path could not make and which cost #204 a bug.
     * @throws cancelled if the token was set
     */
    task<bool> fill();

private:
    reactor&     m_reactor;
    int          m_fd;
    cancel_token m_token;

    // The same 1024 basic_socketbuf uses, and for no better reason than that
    // nobody has measured either.
    char        m_buf[1024];
    std::size_t m_at = 0;
    std::size_t m_end = 0;
    bool        m_closed = false;
};

}
}

#endif // JLIB_SYS_ASYNC_READER_HH
