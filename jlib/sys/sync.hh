/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_SYS_SYNC_HH
#define JLIB_SYS_SYNC_HH

#include <thread>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <exception>
#include <string>
#include <queue>
#include <atomic>
#include <functional>
#include <iostream>
#include <vector>

namespace jlib {
namespace sys {

class sync_exception : public std::exception {
public:
    sync_exception(const std::string& msg = "") {
        m_msg = "sys exception: "+msg;
    }
    virtual ~sync_exception() {}
    virtual const char* what() const noexcept { return m_msg.c_str(); }
protected:
    std::string m_msg;
};
    
template<class T>
void safe_set(T& t, const T& newval, std::mutex& m) {
    std::unique_lock<std::mutex> lock(m);
    
    t = newval;
}
    
template<class T>
T safe_get(const T& t, std::mutex& m) {
    T ret;
    std::unique_lock<std::mutex> lock(m);
    
    ret = t;
    
    return ret;
}
    
template<class T>
class sync {
public:
    typedef T                 value_type;
    typedef value_type&       reference;
    typedef const value_type& const_reference;
    typedef value_type*       pointer;
    typedef value_type* const const_pointer;

    sync() {}
    sync(const_reference val) { set(val); }
    sync(const sync<T>& copy) : sync() { set(copy.get()); }
    
    reference operator()() { return ref(); }
    const_reference operator()() const { return ref(); }

    void operator()(const_reference val) { set(val); }

    operator T() const { return get(); }
    operator std::mutex&() { return m_lock; }

    sync<T>& operator=(const_reference val) { set(val); return *this; }

    value_type get() const {
        return safe_get(m_val, m_lock);
    }

    void set(const_reference val) {
        safe_set(m_val, val, m_lock);
        notify_all();
    }

    reference ref() { return m_val; }
    const_reference ref() const { return m_val; }

    pointer operator->() { return &m_val; }
    const_pointer operator->() const { return &m_val; }

    std::mutex& mutex() { return m_lock; }

    void lock() { m_lock.lock(); }
    void unlock() { m_lock.unlock(); }
    
    void wait(std::unique_lock<std::mutex>& lock) {
        m_cond.wait(lock);
    }
    
    void wait(std::unique_lock<std::mutex>& lock, std::chrono::nanoseconds timeout) {
        m_cond.wait_for(lock, timeout);
    }
    
    void notify() {
        m_cond.notify_one();
    }
    void notify_all() {
        m_cond.notify_all();
    }
    
protected:
    mutable std::condition_variable m_cond;
    mutable std::mutex m_lock;
    mutable T m_val;
};

/**
 * A pool of threads, and functors for them to run.
 *
 * Not a queue.  A queue that passes arbitrary objects between threads is
 * sync<std::queue<T>>, which is right above this and which this is built out
 * of -- so calling the wrapper "queue" as well named the wrong half of it.
 * What it is is a *job* queue: post a functor, and some thread in the pool
 * runs it.
 *
 * ## What it does not do
 *
 * No size(), no wait for idle, no priorities, no futures, no work stealing.  It
 * is twenty lines and its appeal is that it is twenty lines; anything that
 * wants one of those can ask for it then.
 */
class job_queue {
public:
    /** What to do with an exception a job let escape.  See on_error(). */
    typedef std::function<void(const std::exception&)> error_handler;

    /**
     * @param pool_size how many threads.  Clamped to at least one:
     *                  hardware_concurrency() is permitted to return zero, and
     *                  a pool with no threads accepts jobs and runs none.
     */
    job_queue(int pool_size = std::thread::hardware_concurrency()) {
        m_exit = false;
        m_drain = true;

        if(pool_size < 1) pool_size = 1;

        for(int i = 0; i < pool_size; i++) {
            m_pool.push_back(std::thread([this](){ start(); }));
        }
    }

    ~job_queue() {
        stop();
        join();
    }

    job_queue(const job_queue&) = delete;
    job_queue& operator=(const job_queue&) = delete;

    /** Hand a job to the pool.  Ignored once the queue has been stopped. */
    void post(std::function<void()> job) {
        // Before the lock, deliberately: m_exit is atomic, so a stopped queue
        // is not worth contending a mutex to be told about.  It is an
        // optimisation and not a guarantee -- a stop() may land immediately
        // after this reads -- and under drain semantics that window is
        // harmless, because a job queued in it still runs.
        if(m_exit) return;

        std::unique_lock<std::mutex> lock(m_queue);

        m_queue().push(std::move(job));
        m_queue.notify();
    }

    /**
     * What to do with an exception a job let escape.
     *
     * Runs on the worker's thread.  The default writes one line to std::cerr,
     * built whole before it is written -- several workers reporting at once
     * otherwise interleave in the middle of a message.
     *
     * There has to be one.  An exception escaping a thread function calls
     * std::terminate, and it cannot be caught from outside: exceptions do not
     * cross thread boundaries, so the catch has to be in the code that calls
     * the job.  A pool that runs arbitrary functors will be handed one that
     * throws eventually.
     */
    void on_error(error_handler h) {
        std::unique_lock<std::mutex> lock(m_queue);

        m_on_error = h;
    }

    /**
     * Stop taking jobs and let the workers leave.
     *
     * @param drain run what is already queued first.  join() then means "every
     *              job I posted has run", which is the useful guarantee -- and
     *              the destructor calls this, so a queue going out of scope
     *              does not silently discard work.  false leaves as soon as
     *              the job in hand finishes.
     *
     * Idempotent, and safe from any thread including a job's own.  **Not a
     * barrier**: jobs already running keep running, and join() is what waits
     * for them.
     *
     * With drain set, a caller that goes on posting keeps the drain going.
     * That is a caller's mistake and not something this defends against.
     */
    void stop(bool drain = true) {
        {
            // Both under the lock, so a worker that observes m_exit also
            // observes the matching m_drain: they are read together in the
            // predicate below, and two independent atomics give no such
            // guarantee.  m_exit stays atomic all the same, because post()
            // reads it without the lock.
            std::unique_lock<std::mutex> lock(m_queue);

            m_drain = drain;
            m_exit = true;
        }

        m_queue.notify_all();
    }

    /** Wait for every worker to leave.  Idempotent. */
    void join() {
        for(auto& thread : m_pool) {
            if(thread.joinable()) thread.join();
        }
    }

protected:
    void start() {
        for(;;) {
            std::function<void()> job;

            {
                std::unique_lock<std::mutex> lock(m_queue);

                // The predicate, written out, because sync<T>::wait has no
                // overload that takes one.  It has to be a loop and it has to
                // re-check under the lock: a worker that tested m_exit, was
                // descheduled, and resumed after stop() had set the flag and
                // notified would otherwise wait for a notification that has
                // been and gone, and join() would never return.
                while(!m_exit && m_queue().empty())
                    m_queue.wait(lock);

                if(m_queue().empty()) return;        // stopped, nothing left
                if(m_exit && !m_drain) return;       // stopped, abandoning

                // Moved out under the lock so it can be called without one,
                // which is the whole point.  Running it in here -- which is
                // what this did -- made a pool of threads that took turns at
                // one mutex, so exactly one job ran at a time.
                job = std::move(m_queue().front());
                m_queue().pop();
            }

            run(job);
        }
    }

    void run(const std::function<void()>& job) {
        try {
            job();

            return;
        }
        catch(std::exception& e) {
            // The handler is read under the lock, and only here: no data race
            // with on_error(), and no cost at all on the path where nothing
            // throws.
            error_handler h;

            {
                std::unique_lock<std::mutex> lock(m_queue);

                h = m_on_error;
            }

            if(h) h(e);

            return;
        }
        catch(...) {
            // Nothing to hand a handler that takes a std::exception&.
        }
    }

    sync<std::queue<std::function<void()>>> m_queue;
    std::vector<std::thread> m_pool;
    std::atomic<bool> m_exit;
    bool m_drain = true;                  // guarded by m_queue's mutex
    error_handler m_on_error = default_error_handler;

    static void default_error_handler(const std::exception& e) {
        // One write of one whole string; several workers reporting at once
        // otherwise interleave.
        std::cerr << (std::string("jlib::sys::job_queue: a job threw: ") +
                      e.what() + "\n") << std::flush;
    }
};
    
}
}
#endif //JLIB_SYS_SYNC_HH
