/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */

#ifndef JLIB_SYS_RINGBUFFER_HH
#define JLIB_SYS_RINGBUFFER_HH

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace jlib {
namespace sys {

/**
 * A lock-free queue for one producer and one consumer.
 *
 * Written for the audio path, where the consumer is a device callback running
 * under a realtime deadline: it may not allocate, take a lock, or block, so
 * the usual mutex-and-condition queue is not available to it.  Nothing here
 * allocates after construction and nothing waits.
 *
 * Exactly one thread may call write(), and exactly one other may call read().
 * That restriction is what makes it lock-free without a compare-and-swap: each
 * index is written by one thread only and read by both, so a plain load and
 * store with the right ordering is enough.  Two producers would corrupt it.
 *
 * The indices are monotonic counts of items ever written and ever read, rather
 * than positions in the array, and the position is the count modulo the
 * capacity.  Counting rather than pointing removes the usual difficulty that a
 * full buffer and an empty one look alike -- the difference of the counts is
 * the fill, unambiguously, so the whole capacity is usable and there is no
 * spare slot.  The counts wrap eventually; unsigned arithmetic means their
 * difference stays right when they do.
 */
template<typename T>
class ringbuffer {
public:
    typedef std::size_t size_type;

    /**
     * @param capacity items the buffer holds, all of them usable
     */
    explicit ringbuffer(size_type capacity)
        : m_buf(capacity),
          m_written(0),
          m_read(0)
    {
    }

    size_type capacity() const { return m_buf.size(); }

    /**
     * Items waiting to be read.
     *
     * Safe from either thread, and a lower bound from the producer's side:
     * the consumer may have taken more by the time the answer is used, never
     * fewer.  The reverse holds for writable().
     */
    size_type readable() const
    {
        return m_written.load(std::memory_order_acquire) -
               m_read.load(std::memory_order_acquire);
    }

    /** Room for that many more items. */
    size_type writable() const { return capacity() - readable(); }

    bool empty() const { return readable() == 0; }
    bool full() const { return writable() == 0; }

    /**
     * Copy in up to n items, and say how many went.
     *
     * A short write means the buffer filled; it is not an error, and the
     * caller keeps the rest.  Producer thread only.
     */
    size_type write(const T* src, size_type n)
    {
        // The producer owns m_written, so it needs no synchronization to read
        // its own value.  m_read is the consumer's, and acquire here pairs
        // with its release in read() so the space it has freed is visible.
        const size_type written = m_written.load(std::memory_order_relaxed);
        const size_type read = m_read.load(std::memory_order_acquire);

        const size_type count = std::min(n, capacity() - (written - read));
        if(count == 0)
            return 0;

        // At most two runs, since the region can wrap the end of the array.
        const size_type pos = written % capacity();
        const size_type first = std::min(count, capacity() - pos);

        std::copy(src, src + first, m_buf.begin() + pos);
        if(count > first)
            std::copy(src + first, src + count, m_buf.begin());

        // Release, so a consumer that sees the new count also sees the items.
        // Publishing the count before the data is exactly the bug this
        // ordering exists to prevent.
        m_written.store(written + count, std::memory_order_release);

        return count;
    }

    /**
     * Copy out up to n items, and say how many came.
     *
     * A short read means the buffer ran dry.  Consumer thread only.
     */
    size_type read(T* dst, size_type n)
    {
        const size_type read = m_read.load(std::memory_order_relaxed);
        const size_type written = m_written.load(std::memory_order_acquire);

        const size_type count = std::min(n, written - read);
        if(count == 0)
            return 0;

        const size_type pos = read % capacity();
        const size_type first = std::min(count, capacity() - pos);

        std::copy(m_buf.begin() + pos, m_buf.begin() + pos + first, dst);
        if(count > first)
            std::copy(m_buf.begin(), m_buf.begin() + (count - first), dst + first);

        m_read.store(read + count, std::memory_order_release);

        return count;
    }

    /**
     * Discard everything held.
     *
     * Not safe against a running consumer -- it moves the read index, which is
     * the consumer's to move.  Call it only with the consumer stopped, which
     * for the audio path means after the stream has been aborted.
     */
    void clear()
    {
        m_read.store(m_written.load(std::memory_order_acquire),
                     std::memory_order_release);
    }

private:
    std::vector<T> m_buf;

    /** Items ever written.  Producer writes, both read. */
    std::atomic<size_type> m_written;

    /** Items ever read.  Consumer writes, both read. */
    std::atomic<size_type> m_read;
};

}
}

#endif // JLIB_SYS_RINGBUFFER_HH
