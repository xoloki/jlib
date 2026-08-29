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
#include <concepts>
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

    /**
     * Wait until the predicate holds.
     *
     * The form to reach for, and the one this wrapper was missing.  A bare
     * wait() has to be written inside a loop -- against a spurious wakeup, and
     * against a notification that arrived before the wait began -- and a caller
     * who forgets gets a hang that reproduces once a week.  This cannot be got
     * wrong: the condition is stated once, positively, and re-checked under the
     * lock every time.
     *
     * Its absence was load-bearing.  media::Player wanted a predicate wait and
     * used a raw std::condition_variable with its own std::mutex to get one,
     * rather than a sync<T> (Player.hh:144, Player.cc:270); sys::job_queue
     * wrote the loop out by hand.
     *
     * ## Why the constraint
     *
     * Without `requires std::predicate`, this overload swallows the timed one
     * above.  wait(lock, milliseconds(5)) needs a converting constructor to
     * reach std::chrono::nanoseconds -- a user-defined conversion -- while this
     * template matches milliseconds exactly, and an exact template match beats
     * a converting non-template.  So the duration would bind here and fail to
     * compile inside, trying to call a duration.  The constraint excludes
     * anything that is not callable, which puts the timed overload back.
     */
    template<typename Predicate>
        requires std::predicate<Predicate>
    void wait(std::unique_lock<std::mutex>& lock, Predicate pred) {
        m_cond.wait(lock, std::move(pred));
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
 * ## A pool of none runs inline
 *
 * pool_size 0 is not a degenerate case, it is a mode: post() runs the job on
 * the calling thread, then and there.  Everything else behaves the same --
 * post() still swallows what a job throws and hands it to on_error(), stop()
 * and join() still work, a job posted after stop() is still dropped -- so a
 * caller can be written once and made synchronous or concurrent by one number.
 *
 * That is what it is for.  A server that would otherwise carry two code paths,
 * one calling the handler inline and one dispatching it, carries neither: it
 * always posts, and the size decides.
 *
 * Two consequences worth knowing.  A job that posts to its own inline queue
 * *recurses* rather than queueing, so a job that posts itself will not stop.
 * And with no pool nothing is ever queued, so stop(true) has nothing to drain.
 *
 * There is no default size, deliberately.  It was hardware_concurrency(), which
 * is permitted to return 0 -- which used to mean a queue that accepted jobs and
 * ran none, and would now mean a silently synchronous one.  Neither is a thing
 * to arrive at by not choosing; a caller deriving the size from
 * hardware_concurrency() should decide for itself what 0 means.
 *
 * ## What it does not do
 *
 * No size(), no wait for idle, no priorities, no futures, no work stealing, no
 * bound on how deep the queue may get.  It is twenty lines and its appeal is
 * that it is twenty lines; anything that wants one of those can ask for it
 * then.
 */
class job_queue {
public:
    /** What to do with an exception a job let escape.  See on_error(). */
    typedef std::function<void(const std::exception&)> error_handler;

    /**
     * @param pool_size how many threads, or 0 to run each job on the thread
     *                  that posts it.  No default: see the note above.
     */
    explicit job_queue(int pool_size) {
        m_exit = false;
        m_drain = true;

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

    /**
     * Hand a job to the pool, or run it here if there is no pool.
     *
     * Ignored once the queue has been stopped, either way.  And either way it
     * does not throw what the job threw: an exception has nowhere to go from a
     * worker thread, so run() catches it and on_error() gets it, and a caller
     * whose behaviour changed with the pool size would defeat the point of
     * being able to set that size to zero.
     */
    void post(std::function<void()> job) {
        // Before the lock, deliberately: m_exit is atomic, so a stopped queue
        // is not worth contending a mutex to be told about.  It is an
        // optimisation and not a guarantee -- a stop() may land immediately
        // after this reads -- and under drain semantics that window is
        // harmless, because a job queued in it still runs.
        if(m_exit) return;

        // m_pool is written once, in the constructor, and never touched again,
        // so reading it here needs no lock.
        if(m_pool.empty()) {
            run(job);

            return;
        }

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

                // Re-checked under the lock, every time, which is what the
                // predicate form guarantees: a worker that tested m_exit
                // outside the lock and then waited would miss a stop() that
                // landed in between -- a notification that has been and gone --
                // and join() would never return.
                m_queue.wait(lock, [this] {
                    return m_exit || !m_queue().empty();
                });

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
