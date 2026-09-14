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
#ifndef JLIB_SYS_REACTOR_HH
#define JLIB_SYS_REACTOR_HH

#include <jlib/sys/pipe.hh>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <queue>
#include <string>
#include <vector>

namespace jlib {
namespace sys {

/**
 * The thing that does the waiting: kqueue, epoll or poll.
 *
 * Declared and not installed.  Which one is running is not part of the
 * contract; reactor::backend() is all a caller may ask.
 */
class reactor_backend;

/**
 * One thread, many descriptors: wait for readiness and call something.
 *
 * jlib has had four ways to wait and no way to wait on two things at once.
 * Servent blocks on its command pipe, ASServent on a condition variable,
 * job_queue on a queue depth, server on poll(2) over a listener and a wake
 * pipe -- and nothing in the library can wait for a request *and* a socket.
 * ASServent says so in as many words: its responses travel down a pipe rather
 * than a queue purely so that somebody else's event loop can select on the
 * read end.  This is that loop, written down.
 *
 * ## What it is not
 *
 * It is not asio.  There is no async_read, no completion handler, no buffer,
 * no error_code, no executor, no cancellation.  It reports that a descriptor
 * is ready and calls a function; what that function does with the descriptor
 * is the caller's business, and is always an ordinary blocking-style call on a
 * descriptor already known to be ready.
 *
 * That restraint is the design.  The library has not chosen between stackful
 * coroutines and C++20's stackless ones, and this must not choose for it.
 * Both resume the same way -- a fiber's callback switches to its context, a
 * coroutine's resumes its handle -- so a reactor that only ever says "ready,
 * here is your function" serves either:
 *
 *     void await_suspend(std::coroutine_handle<> h) {
 *         m_r.once(m_fd, reactor::READ,
 *                  [h](reactor::token, int, reactor::event_type) {
 *                      h.resume();
 *                  });
 *     }
 *
 * The fork is not about readiness.  It is about *operations and completions* --
 * who owns the buffer, how an error arrives -- and that layer is deliberately
 * absent.  See once(), which is the resumption primitive.
 *
 * ## Level-triggered, and that is a contract
 *
 * A registration stays ready for as long as the descriptor is.  A callback
 * that reads half of what is waiting will be called again, which is what lets
 * basic_socketbuf::underflow -- which reads once into a fixed buffer and
 * cannot accumulate across two calls -- sit on top of this unchanged.  Both
 * native backends are level-triggered by default and stay that way: no
 * EPOLLET, no EV_CLEAR.  Edge triggering would oblige every callback to drain
 * to EAGAIN, which is an operation layer, which is the thing not being built.
 *
 * The cost is that a callback which neither consumes nor disables its
 * registration spins.  ERROR and HANGUP are where that bites, because nothing
 * consumes them; see the note on those constants.
 *
 * ## One thread
 *
 * run() and run_one() are called by exactly one thread.  add(), once(),
 * modify(), remove(), after(), every() and cancel() are callable only from
 * that thread, or from any thread before the loop starts.  post(), wake() and
 * stop() are callable from any thread at any time, and post() is how another
 * thread reaches the rest of the interface.
 *
 * That is a choice with teeth, and what it buys is remove()'s guarantee: once
 * remove() returns, the callback will not run again -- not later in this
 * dispatch pass, not ever.  A reactor several threads could run() would have
 * to weaken that to "will not run again after the callbacks already in flight
 * finish", and a caller who freed what the callback captured on the strength
 * of the stronger reading would have a use-after-free.  Concurrency here is
 * job_queue, which already exists and already runs inline at size zero.
 *
 * Callbacks run with none of this object's locks held, so a callback may call
 * anything on its own reactor, including add(), remove() and stop().  It may
 * **not** call run() or run_one(); a nested pass throws.
 *
 * ## Not across a fork
 *
 * A kqueue descriptor is not usefully inherited and an epoll one is inherited
 * *shared*.  Construct a reactor in the child if the child needs one.
 */
class reactor {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = "reactor exception: " + msg;
        }
        virtual ~exception() {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }
    protected:
        std::string m_msg;
    };

    /**
     * What a registration is interested in, and what it is told.
     *
     * Its own bits, not POLLIN and POLLOUT.  sys::pipe aliases its event_type
     * straight onto poll(2)'s constants, which was free when poll(2) was the
     * only implementation and is a leak of one now that there are three.
     */
    typedef unsigned int event_type;

    static const event_type NONE  = 0;
    static const event_type READ  = 1u << 0;
    static const event_type WRITE = 1u << 1;

    /**
     * Delivered, never requested.
     *
     * A registration that asked only for WRITE still gets these.  Every
     * backend reports them unasked -- poll(2) in revents, epoll always,
     * kqueue as EV_EOF and EV_ERROR -- so pretending they were optional would
     * mean dropping them.
     *
     * **HANGUP does not mean there is nothing to read.**  A peer that has sent
     * data and closed gives HANGUP with the data still in the socket buffer,
     * and a callback that treats it as end of stream loses the last read.
     * That is the same mistake basic_tlsbuf::underflow made with SSL_read one
     * layer down, and it is written here rather than left for a reader to
     * discover.
     *
     * Neither is consumed by reading, so a callback that gets one and does
     * nothing is called again immediately, forever.  Remove the registration,
     * or disable it with modify(t, NONE).
     */
    static const event_type ERROR  = 1u << 2;
    static const event_type HANGUP = 1u << 3;

    /**
     * A registration, and never a descriptor.
     *
     * Descriptor numbers are reused the moment they are closed, so a table
     * keyed on one hands a stale removal to a live connection.  This is a
     * serial that is never reused, and the backends carry it in the 64-bit
     * user word that kqueue and epoll both provide.
     */
    enum class token : std::uint64_t { none = 0 };
    enum class timer_token : std::uint64_t { none = 0 };

    /**
     * @param t this registration, so a callback can remove itself without the
     *          back-patching a captured token would need.  For once() it names
     *          a registration that is already gone, and remove() on it is a
     *          no-op.
     */
    typedef std::function<void(token t, int fd, event_type events)> ready_handler;

    /** @param t this timer, so a repeating one can cancel itself. */
    typedef std::function<void(timer_token t)> timer_handler;

    typedef std::function<void()> job;

    /**
     * What to do with an exception a callback let escape.
     *
     * The rest of the pass still runs, as job_queue and server do and for the
     * same reason: a loop a single bad callback can end is a loop that stops
     * serving everyone.  It matters more here than there, because a resumed
     * fiber or coroutine throwing is ordinary.  The default writes one line to
     * std::cerr, assembled whole before it is written.
     */
    typedef std::function<void(const std::exception&)> error_handler;

    /**
     * Wait with no deadline.
     *
     * Negative, not zero -- and this is the one place the reactor breaks with
     * listener::accept, basic_socketbuf's timeouts and server::serve_one, all
     * of which spell "forever" as 0.  Zero has to mean "do not wait" here,
     * because a reactor embedded in somebody else's loop -- a GUI's, which is
     * the case ASServent's response pipe exists for -- needs to drain what is
     * ready and hand control straight back.  A named constant makes the
     * divergence impossible to write by accident.
     */
    static constexpr std::chrono::nanoseconds forever{-1};

    /**
     * Keeps run() running when there is nothing registered.
     *
     * run() returns when the last registration, timer and job is gone, so a
     * finite amount of work terminates rather than hanging.  A thread that
     * means to post() later, into a reactor that may momentarily have nothing,
     * holds one of these.
     */
    class work {
    public:
        explicit work(reactor& r);
        ~work();
        work(const work&) = delete;
        work& operator=(const work&) = delete;
    private:
        reactor& m_reactor;
    };

    reactor();

    /**
     * Out of line, because m_backend points at an incomplete type here.  Which
     * costs the implicit move operations -- and this holds a pipe and a kernel
     * descriptor, so it was never going to be movable.
     */
    ~reactor();

    reactor(const reactor&) = delete;
    reactor& operator=(const reactor&) = delete;

    /**
     * Watch a descriptor until it is removed.
     *
     * @param events READ, WRITE, or both.  ERROR and HANGUP are ignored here
     *               and delivered regardless.
     * @return a token, never token::none.
     *
     * The descriptor is not taken over: this neither closes it nor changes its
     * flags, and in particular **does not make it non-blocking**.  A caller
     * that wants that does it itself, and knows what it is doing to whatever
     * else reads the descriptor -- for a TLS stream today the answer is that
     * it must not, and sslstream.hh says why.
     *
     * A registration added from inside a callback is not dispatched until the
     * next pass; this one's ready list was decided before it existed.
     *
     * Two registrations on one descriptor throws: the poll backend can express
     * it and the other two cannot agree on what it means.
     */
    token add(int fd, event_type events, ready_handler h);

    /**
     * Watch it once.  The registration is gone before the callback runs.
     *
     * The resumption primitive: a fiber that wants to sleep until a descriptor
     * is readable, or a coroutine awaiter's await_suspend, is this call and
     * nothing else.  Written out of add() rather than left to a callback that
     * removes itself, because the ordering matters -- removing first is what
     * makes it safe for the callback to re-register the same descriptor, which
     * is exactly what the next read in the same fiber will do.
     */
    token once(int fd, event_type events, ready_handler h);

    /**
     * Change what a registration is interested in.  NONE disables it.
     *
     * Throws on an unknown token.  Unlike remove(), there is no benign reading
     * of modifying something that is gone.
     */
    void modify(token t, event_type events);

    /**
     * Stop watching.
     *
     * **When this returns the callback will not run again**, including not
     * later in the pass it was called from -- the ready list is resolved token
     * by token as it is walked, so a removal made by an earlier callback
     * suppresses a later one.  That guarantee is what makes it safe to destroy
     * whatever the callback captured, and it is why the loop is single
     * threaded.
     *
     * Safe from inside the callback's own invocation: the entry is held by
     * shared_ptr for the duration of the call, so removing yourself does not
     * free the closure you are running inside.
     *
     * An unknown token is a no-op, not an error.  A token names something that
     * happened, and two clean-up paths both reaching for it is ordinary --
     * job_queue::stop() is idempotent for the same reason.
     */
    void remove(token t);

    /** Run h once, no sooner than d from now.  Never early; see reactor.cc. */
    timer_token after(std::chrono::nanoseconds d, timer_handler h);

    /**
     * Run h every d.
     *
     * The period is measured from each firing rather than from the deadline,
     * so a slow callback drops ticks instead of accumulating a backlog it can
     * never work off.
     */
    timer_token every(std::chrono::nanoseconds d, timer_handler h);

    /** An unknown or already-fired timer is a no-op.  See remove(). */
    void cancel(timer_token t);

    /**
     * Run a functor on the reactor's thread, at the top of the next pass.
     *
     * Safe from any thread, and the only way another thread may reach add(),
     * remove() or a timer.  FIFO.  A job posted from inside a callback runs on
     * the *next* pass, not this one -- a job that posts itself would otherwise
     * never let the pass end.
     *
     * Jobs run before the ready descriptors, which is what makes a posted
     * remove() take effect on the same pass it is delivered.
     */
    void post(job j);

    /** Make a blocked run_one() return.  Safe from any thread.  Coalesced. */
    void wake();

    /**
     * One wait and one dispatch pass.
     *
     * @param timeout how long to wait for something to happen.  Zero returns
     *                at once with whatever was already ready; forever has no
     *                deadline.  A timer due sooner shortens it.
     * @return whether anything was dispatched -- a job, a callback or a timer.
     *
     * Throws if called from inside a callback.
     */
    bool run_one(std::chrono::nanoseconds timeout = forever);

    /**
     * run_one() until stop(), or until there is nothing left to do.
     *
     * "Nothing left" is no registrations, no timers, no queued jobs and no
     * work guard, checked after each pass, so a finite amount of work ends
     * rather than hanging.  A thread that means to post() into a momentarily
     * idle reactor holds a reactor::work.
     *
     * **One-shot.**  stop() is sticky and this does not clear it, for the
     * reason server::run() gives: a loop that could be restarted invites a
     * restart that silently drops everything registered before the stop.  A
     * reactor is cheap; make another.
     */
    void run();

    /** Ask run() to return.  Safe from any thread, including from a callback. */
    void stop();

    bool stopped() const;

    /** Registrations, not counting the wake pipe, which is this object's own. */
    std::size_t count() const;

    std::size_t timers() const;

    /** "kqueue", "epoll" or "poll".  For a log line, and for a test's detail. */
    const char* backend() const;

    /**
     * Whether the calling thread is the one turning this reactor.
     *
     * True before the loop has ever run, because there is no other thread to
     * be wrong about yet and a caller registering during setup is the normal
     * case.  After the first pass it is the thread that made it.
     *
     * Exists because a coroutine can now be moved to a worker -- see
     * sys::on_pool -- and an await made from there cannot register directly.
     * until_ready asks this and posts instead.
     */
    bool on_reactor_thread() const;

    void on_error(error_handler h);

private:
    struct entry {
        token         id     = token::none;
        int           fd     = -1;
        event_type    events = NONE;
        bool          once   = false;
        ready_handler handler;
    };

    struct timer {
        timer_token                           id = timer_token::none;
        std::chrono::steady_clock::time_point due{};
        std::chrono::nanoseconds              period{0};   // zero for one-shot
        timer_handler                         handler;
    };

    void drain_wake();
    void fire_timers();
    std::chrono::nanoseconds next_wait(std::chrono::nanoseconds budget) const;
    void report(const std::exception& e);

    std::unique_ptr<reactor_backend> m_backend;

    // Ordered rather than hashed: an enum class already has the relational
    // operators and would need a std::hash specialisation it does not have.
    //
    // Held by shared_ptr so that a callback which removes its own registration
    // does not destroy the std::function it is running inside of.  The
    // dispatch loop keeps a copy across every call; the map entry may go at
    // any point during it.
    std::map<token, std::shared_ptr<entry> > m_entries;

    // Ordered by deadline, so the front is what shortens the next wait, and
    // cancel() is a find and an erase rather than a tombstone that a run of
    // cancelled repeating timers would grow without bound.
    std::multimap<std::chrono::steady_clock::time_point,
                  std::shared_ptr<timer> >   m_by_due;
    std::map<timer_token, std::shared_ptr<timer> > m_timers;

    // Reused across passes rather than allocated per pass.
    std::vector<std::pair<token, event_type> > m_ready;

    std::uint64_t m_next_token = 1;
    std::uint64_t m_next_timer = 1;

    // Non-blocking at both ends, as server's was, so a wake() called a
    // thousand times cannot block once the pipe fills.
    pipe  m_wake{false, false};
    token m_wake_token = token::none;

    // wake() writes a byte only on the 0 -> 1 edge, so a flood of post()s
    // costs one write and one read rather than one each.  Cleared at the top
    // of a pass, so a post() landing during one writes again and the next wait
    // returns at once.
    std::atomic<bool> m_woken{false};

    mutable std::mutex m_jobs_lock;     // covers m_jobs only
    std::queue<job>    m_jobs;
    std::vector<job>   m_running;       // this pass's batch, swapped out

    std::atomic<bool> m_stop{false};
    std::atomic<int>  m_work{0};

    bool m_dispatching = false;         // reactor thread only

    // Whose thread this is.  Claimed by the first pass rather than by the
    // constructor: a reactor is routinely built on one thread and run on
    // another, and the constructor's thread is not the interesting one.
    std::atomic<bool>       m_claimed{false};
    std::thread::id         m_owner;

    error_handler m_on_error;
};

}
}

#endif // JLIB_SYS_REACTOR_HH
