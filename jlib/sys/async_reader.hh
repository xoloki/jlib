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
 * A buffer that something can top up without blocking a thread.
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
 * ## Where a descriptor is not the whole story
 *
 * fill() is the only virtual, and it is virtual because TLS needs a different
 * answer to "get me more".  A TLS connection has **two** readiness questions
 * -- is the socket readable, and does the SSL already hold decrypted plaintext
 * that nobody has taken -- and a reactor answers only the first.  Anything
 * that asks the descriptor alone waits for bytes that have already arrived.
 *
 * The buffer, get() and take() are the same either way, so they live here and
 * are not virtual.  **No framing function knows which it has**: read_head,
 * read_body and imap::read all take an async_reader& and were not touched when
 * the TLS one arrived.
 */
class async_reader {
public:
    /** What get() answers when the buffer is empty.  Not a valid octet. */
    static const int empty = -2;

    /** Polymorphic base.  The rest are deleted rather than defaulted: a
     *  half-copied buffer with a live descriptor behind it is not a thing. */
    virtual ~async_reader() {}

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
     * a chunk, a literal.  get() in a loop would be correct and would cost a
     * call per octet; a megabyte body is a megabyte of them.
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

    /** Whether the source has ended and the buffer is spent. */
    bool spent() const { return m_closed && m_at == m_end; }

    /**
     * Get more.  The only suspension point in any framing function built on
     * this, and the only thing an implementation has to supply.
     *
     * @return false at end of stream, so a caller can tell "nothing more is
     *         coming" from "nothing yet" -- which is the distinction the
     *         synchronous path could not make and which cost #204 a bug.
     * @throws cancelled if a token was set
     */
    virtual task<bool> fill() = 0;

protected:
    async_reader() {}

    /** For an implementation that has just read n octets into buf(). */
    void filled(std::size_t n) { m_at = 0; m_end = n; }

    void ended() { m_closed = true; }

    // The same 1024 basic_socketbuf uses, and for no better reason than that
    // nobody has measured either.
    static const std::size_t BUF_SIZE = 1024;

    char* buf() { return m_buf; }
    static std::size_t buf_size() { return BUF_SIZE; }

    bool m_closed = false;

private:
    char        m_buf[BUF_SIZE];
    std::size_t m_at = 0;
    std::size_t m_end = 0;
};

/**
 * An async_reader over a descriptor.
 *
 * ## It does not set O_NONBLOCK, deliberately
 *
 * The reactor reports a descriptor readable and a read on a ready descriptor
 * returns without blocking, so the flag is not needed -- and leaving it alone
 * is what keeps listener.cc's unconditional strip intact.  That line exists
 * because BSD and macOS *inherit* the flag on an accepted descriptor and Linux
 * does not, and its comment calls the divergence "the worst way for a
 * difference like this to be found".
 *
 * async_tls_reader is the one that must set it, and says why.
 */
class async_fd_reader : public async_reader {
public:
    /**
     * @param fd not taken over: this neither closes it nor changes its flags
     * @param t  cancels a fill() that is waiting; see cancel_token
     */
    async_fd_reader(reactor& r, int fd, cancel_token t = cancel_token())
        : m_reactor(r), m_fd(fd), m_token(t) {}

    task<bool> fill();

private:
    reactor&     m_reactor;
    int          m_fd;
    cancel_token m_token;
};

}
}

#endif // JLIB_SYS_ASYNC_READER_HH
