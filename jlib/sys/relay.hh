/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2002 Joey Yandle <xoloki@gmail.com>
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
#ifndef JLIB_SYS_RELAY_HH
#define JLIB_SYS_RELAY_HH

#include <jlib/sys/await.hh>
#include <jlib/sys/pipe.hh>
#include <jlib/sys/reactor.hh>
#include <jlib/sys/sync.hh>
#include <jlib/sys/task.hh>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

namespace jlib {
namespace sys {

/**
 * A long action on its own thread, reporting back through a descriptor.
 *
 * ## The shape it fills
 *
 * A reactor's thread must not block, and a job_queue's threads are for
 * *handling requests* -- one held for the length of an inference or a
 * subprocess is one not handling any, which is the same mistake as blocking
 * the reactor and merely further from the symptom.
 *
 * So anything that drives a long external action gets a thread of its own and
 * reports back through something the reactor can already wait on.  That is a
 * descriptor, and a pipe is the cheapest one.  The byte's *value* means
 * nothing; that one arrived is the whole message.
 *
 * ## Why not ASServent
 *
 * sys::ASServent is this pattern and predates it by twenty-five years, with
 * the same reasoning written on get_response_reader().  What it does not have
 * is **correlation**: one response pipe per servent, shared by every request
 * it has ever been given, so two actions in flight each wake on the other's
 * output.  A relay is one per action, which is the whole difference.
 *
 * ## What crosses the thread boundary
 *
 * Items one way, under a mutex, with a byte down the pipe to say there is
 * something to take.  A stop flag the other way, read between items, which is
 * how an action gets abandoned when nobody is left to want it.
 *
 * Nothing else.  In particular the worker must not touch whatever the reactor
 * side is doing with the items it has already taken.
 */
template<typename Item>
class relay {
public:
    relay() : m_pipe(false, false) {}

    ~relay() { join(); }

    relay(const relay&) = delete;
    relay& operator=(const relay&) = delete;

    /** The descriptor a coroutine waits on.  Level-triggered; see take(). */
    int reader() { return m_pipe.get_reader(); }

    /**
     * Run `work` on a thread of its own.
     *
     * It is handed this relay and should emit() as it goes.  finish() is
     * called for it when it returns, and fail() if it throws, so a worker that
     * dies cannot leave a reader waiting for ever.
     */
    void start(std::function<void(relay&)> work) {
        m_thread = std::thread([this, work]{
            try { work(*this); }
            catch(std::exception& e) { fail(e.what()); }
            catch(...) { fail("the work failed"); }

            finish();
        });
    }

    /** Worker side: one item of output. */
    void emit(Item item) {
        {
            std::lock_guard<std::mutex> guard(m_items.mutex());

            m_items().push_back(std::move(item));
        }

        wake();
    }

    /** Worker side: nothing more is coming.  Idempotent. */
    void finish() {
        m_done.store(true);

        wake();
    }

    /** Worker side: nothing more is coming, and it ended badly. */
    void fail(const std::string& why) {
        {
            std::lock_guard<std::mutex> guard(m_why.mutex());

            if(m_why().empty()) m_why() = why;
        }

        m_done.store(true);

        wake();
    }

    /**
     * Reactor side: everything emitted since the last call.
     *
     * Drains the pipe too, because the descriptor is level-triggered: a byte
     * left behind makes the next wait return immediately, for ever.
     *
     * **Draining is also why the caller cannot simply wait again**: see pump(),
     * whose ordering exists because this call and done() are two separate
     * facts about a thread that is still running.
     */
    std::deque<Item> take() {
        char buf[64];

        while(::read(m_pipe.get_reader(), buf, sizeof buf) > 0)
            ;

        std::deque<Item> out;

        {
            std::lock_guard<std::mutex> guard(m_items.mutex());

            out.swap(m_items());
        }

        return out;
    }

    /** Whether the worker has finished.  See pump() before using it. */
    bool done() const { return m_done.load(); }

    /** Reactor side: ask the work to stop, because nobody is reading. */
    void stop() { m_stop.store(true); }

    /** Worker side, between items. */
    bool wanted() const { return !m_stop.load(); }

    /** Empty unless the work threw or called fail(). */
    std::string why() {
        std::lock_guard<std::mutex> guard(m_why.mutex());

        return m_why();
    }

    /** Stop the work and wait for its thread.  Idempotent; the destructor. */
    void join() {
        stop();

        if(m_thread.joinable()) m_thread.join();
    }

private:
    void wake() {
        const char one = 1;

        // Non-blocking at both ends: a pipe already full has said everything
        // this needed it to say.
        if(::write(m_pipe.get_writer(), &one, 1) < 0) { /* full is fine */ }
    }

    pipe                          m_pipe;
    std::thread                   m_thread;
    sync<std::deque<Item> >       m_items;
    sync<std::string>             m_why;
    std::atomic<bool>             m_done{false};
    std::atomic<bool>             m_stop{false};
};

/**
 * Drive a relay to completion, doing something with each item.
 *
 * ## The ordering, which is the whole of this function
 *
 * take() and done() are two separate facts about a thread that is still
 * running, and the order they are read in decides what gets lost.
 *
 * **The hole that matters, because its window is unbounded.**  Ask done()
 * *after* taking and then stop, and everything the worker emitted while the
 * reader was busy with the previous batch is dropped -- and "busy" is however
 * long the caller's `each` takes, which for a token being written to a socket
 * is a syscall and for a slow client is longer.  The fix is to **take once
 * more after seeing done**: finish() runs after every emit, so once done is
 * true one further take sees everything.
 *
 * **The hole that is only theoretical.**  Reading done() *before* take() would
 * mean a finish() landing between those two statements sets the flag, writes
 * its byte, and has the byte drained by the take() that follows -- leaving the
 * loop waiting for a wake-up that is already gone.  That window is two
 * adjacent statements wide.  It is avoided here by construction and, unlike
 * the first, **there is no test for it**: sys_relay_test provokes the first
 * deliberately and could not provoke this one at all.
 *
 * @param each  called with each item; returning false abandons the rest
 * @return      whether every item was consumed, as opposed to `each` giving up
 */
template<typename Item, typename Each>
task<bool> pump(relay<Item>& r, reactor& rc, Each each) {
    for(;;) {
        std::deque<Item> items = r.take();

        for(Item& item : items) {
            if(!co_await each(item)) co_return false;
        }

        if(r.done()) {
            std::deque<Item> rest = r.take();

            for(Item& item : rest) {
                if(!co_await each(item)) co_return false;
            }

            co_return true;
        }

        co_await readable(rc, r.reader());
    }
}

}
}

#endif
