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
#ifndef JLIB_SYS_AWAIT_HH
#define JLIB_SYS_AWAIT_HH

#include <jlib/sys/reactor.hh>
#include <jlib/sys/sync.hh>
#include <jlib/sys/task.hh>

#include <chrono>
#include <exception>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace jlib {
namespace sys {

/**
 * What a suspended operation throws when it is cancelled rather than completed.
 *
 * Its own type, not reactor::exception, because a caller catching it is doing
 * something entirely different from a caller handling an I/O failure: one is
 * shutting down, the other is reporting.
 */
class cancelled : public std::exception {
public:
    cancelled(const std::string& what = "the operation was cancelled")
        : m_msg(what) {}

    virtual ~cancelled() {}

    virtual const char* what() const noexcept { return m_msg.c_str(); }

protected:
    std::string m_msg;
};

namespace detail {

/** One suspended operation, as much of it as a cancellation needs to reach. */
struct waiter {
    reactor*                r   = 0;
    reactor::token          reg = reactor::token::none;
    std::coroutine_handle<> h;

    // Set by whichever gets there first -- the descriptor becoming ready, the
    // cancellation, or the frame being destroyed -- so the losers do nothing.
    //
    // Atomic since the pool hop: a resume can be pending on a worker while the
    // reactor's thread cancels, and a plain bool read from both is a race.
    std::atomic<bool>       finished{false};
};

}

/**
 * A shared "stop what you are doing".
 *
 * ## It ends the wait, it does not merely mark it
 *
 * The first version set a flag and left the await registered, so a cancelled
 * read on a peer that never speaks stayed parked until the descriptor happened
 * to become ready -- which for a slow-loris is never.  Four write-ups in a row
 * said so under "what this does not establish"; this is that gap closed.
 *
 * request() now walks the operations suspended on the token, removes each
 * one's registration from the reactor, and resumes it.  await_resume() then
 * throws cancelled, so the caller sees a throw at the co_await rather than a
 * wait that never ends.
 *
 * ## A default-constructed token never fires, and that is deliberate
 *
 * The default is the one every readable() and writable() gets when a caller
 * does not pass one, so it has to be free: no allocation, and no bookkeeping
 * at the suspension point.  A token that could never be requested needs
 * neither.
 *
 * The footgun that avoids -- a default token that silently does nothing --
 * is closed by request() throwing on one.  Call create() for a live token.
 *
 * ## One thread
 *
 * request() resumes coroutines, so it may be called only from the reactor's
 * thread: from a timer, from a callback, or before the loop starts.  That is
 * the same rule the reactor states for add() and remove(), and for the same
 * reason.  Another thread cancels by post()ing.
 */
class cancel_token {
public:
    /** Never fires.  Free; see the note above. */
    cancel_token() {}

    /** A token that can actually be requested. */
    static cancel_token create() {
        cancel_token t;

        t.m_state = std::make_shared<state>();

        return t;
    }

    /** Whether this is a token that could ever fire. */
    bool live() const { return bool(m_state); }

    bool requested() const { return m_state && m_state->requested; }

    /**
     * Cancel, and **end** every wait suspended on this token.
     *
     * Idempotent.  Each resumed coroutine runs to its next suspension inside
     * this call, exactly as it would inside a reactor callback -- which is
     * where a cancel usually comes from.
     *
     * @throws cancelled_misuse on a default-constructed token, which can have
     *         no waiters and would silently do nothing
     */
    void request();

    /**
     * Shares the flag, not a copy of it.
     *
     * So a token handed to a nested operation is the same token -- cancelling
     * the outer one cancels everything below it, which is what a connection
     * teardown means.
     */
    cancel_token(const cancel_token&) = default;
    cancel_token& operator=(const cancel_token&) = default;

    /** Thrown by request() on a token that can never fire. */
    class misuse : public std::exception {
    public:
        virtual ~misuse() {}
        virtual const char* what() const noexcept {
            return "cancel_token::request() on a default-constructed token, "
                   "which never fires; use cancel_token::create()";
        }
    };

    /** Attach a suspended operation.  Called by until_ready. */
    void join(const std::shared_ptr<detail::waiter>& w) {
        if(m_state) m_state->waiters.push_back(w);
    }

private:
    struct state {
        bool requested = false;

        // Weak, because a waiter is owned by the coroutine frame it lives in
        // and may be destroyed while the token outlives it.  A wait that has
        // already finished locks to nothing and is skipped.
        std::vector<std::weak_ptr<detail::waiter> > waiters;
    };

    std::shared_ptr<state> m_state;
};

inline void cancel_token::request() {
    if(!m_state) throw misuse();

    if(m_state->requested) return;

    m_state->requested = true;

    // Copied out first, and the list cleared, because resuming a coroutine
    // runs it to its next suspension -- which may register a new wait on this
    // same token and invalidate anything held across the loop.
    std::vector<std::shared_ptr<detail::waiter> > live;

    for(std::size_t i = 0; i < m_state->waiters.size(); i++) {
        if(std::shared_ptr<detail::waiter> s = m_state->waiters[i].lock())
            live.push_back(s);
    }

    m_state->waiters.clear();

    for(std::size_t i = 0; i < live.size(); i++) {
        detail::waiter& w = *live[i];

        if(w.finished.exchange(true)) continue;

        // Taken out of the reactor before resuming, so the descriptor becoming
        // ready afterwards finds nothing and cannot resume a second time.
        if(w.reg != reactor::token::none) {
            w.r->remove(w.reg);

            w.reg = reactor::token::none;
        }

        w.h.resume();
    }
}

/**
 * Suspend until a descriptor is ready, or until the token is cancelled.
 *
 * The bridge between a coroutine and the reactor, and it is reactor::once()
 * doing the job it was written for: a fiber's callback would switch to its
 * context and this resumes a handle, which is why the reactor could be built
 * before the choice between them was made.
 *
 * @throws cancelled if the token was set before or during the wait
 */
class until_ready {
public:
    until_ready(reactor& r, int fd, reactor::event_type events,
                cancel_token t = cancel_token())
        : m_reactor(r), m_fd(fd), m_events(events), m_token(t) {}

    /**
     * Unregister, if this is being destroyed while still parked.
     *
     * **This is what makes destroying a suspended task safe.**  The awaiter is
     * a temporary whose lifetime the coroutine extends across the suspension,
     * so it lives *in the frame* -- and destroying the frame destroys it.
     * Without this, the reactor kept a registration holding a handle to freed
     * memory and resumed it when the descriptor next became ready, which ASan
     * reports as a heap-use-after-free inside the coroutine.
     *
     * **Must run on the reactor's thread**, because remove() may only be
     * called from there.
     */
    ~until_ready() {
        if(m_waiter) {
            // Claimed first, so a callback or a posted registration that runs
            // after this finds the wait already over and does nothing.  That
            // is what makes destroying a frame whose registration has not been
            // made yet safe.
            const bool was = m_waiter->finished.exchange(true);

            if(!was && m_waiter->reg != reactor::token::none) {
                if(m_reactor.on_reactor_thread()) {
                    m_reactor.remove(m_waiter->reg);
                }
                else {
                    // remove() is reactor-thread-only, and this frame may be
                    // being destroyed on a worker.
                    reactor& r = m_reactor;
                    const reactor::token t = m_waiter->reg;

                    r.post([&r, t]{ r.remove(t); });
                }
            }

            return;
        }

        if(m_registration != reactor::token::none)
            m_reactor.remove(m_registration);
    }

    until_ready(const until_ready&) = delete;
    until_ready& operator=(const until_ready&) = delete;

    /**
     * A token already set does not suspend at all.
     *
     * Which matters: a loop that cancels between iterations would otherwise
     * register, suspend, and wait for a readiness that may never come.
     */
    bool await_ready() const { return m_token.requested(); }

    void await_suspend(std::coroutine_handle<> h) {
        // **Off the reactor's thread**, which on_pool makes possible.  once()
        // may only be called from the thread turning the reactor, so the
        // registration is posted rather than made -- and the consequence,
        // which the header states, is that this resumes on the reactor's
        // thread rather than where it suspended.
        if(!m_reactor.on_reactor_thread()) {
            m_waiter = std::make_shared<detail::waiter>();

            m_waiter->r = &m_reactor;
            m_waiter->h = h;

            const std::weak_ptr<detail::waiter> w = m_waiter;

            reactor& r = m_reactor;
            const int fd = m_fd;
            const reactor::event_type events = m_events;

            m_reactor.post([w, &r, fd, events]{
                const std::shared_ptr<detail::waiter> s = w.lock();

                // The frame went away between the post and this.
                if(!s || s->finished.load()) return;

                s->reg = r.once(fd, events,
                                [w](reactor::token, int, reactor::event_type) {
                                    const std::shared_ptr<detail::waiter> t =
                                        w.lock();

                                    if(!t) return;

                                    t->reg = reactor::token::none;

                                    if(t->finished.exchange(true)) return;

                                    t->h.resume();
                                });
            });

            if(m_token.live()) m_token.join(m_waiter);

            return;
        }

        if(!m_token.live()) {
            // Nothing can cancel this, so none of the bookkeeping below is
            // reachable -- and this is the common path, taken by every
            // readable() and writable() that a caller did not hand a token.
            m_registration =
                m_reactor.once(m_fd, m_events,
                               [this, h](reactor::token, int,
                                         reactor::event_type) {
                                   m_registration = reactor::token::none;

                                   h.resume();
                               });

            return;
        }

        m_waiter = std::make_shared<detail::waiter>();

        m_waiter->r = &m_reactor;
        m_waiter->h = h;

        // A copy for the callback, so the waiter outlives this object if the
        // frame goes away between the readiness and the dispatch.
        const std::shared_ptr<detail::waiter> w = m_waiter;

        m_waiter->reg =
            m_reactor.once(m_fd, m_events,
                           [w](reactor::token, int, reactor::event_type) {
                               // once() has already removed it.
                               w->reg = reactor::token::none;

                               // The cancellation got here first and has
                               // already resumed this.
                               if(w->finished.exchange(true)) return;

                               w->h.resume();
                           });

        m_token.join(m_waiter);
    }

    /**
     * @throws cancelled if the token was set
     *
     * The one line that makes every caller cancellation-aware.
     */
    void await_resume() const {
        if(m_token.requested()) throw cancelled();
    }

private:
    reactor&            m_reactor;
    int                 m_fd;
    reactor::event_type m_events;
    cancel_token        m_token;

    // One or the other.  m_waiter when the token is live and a cancellation
    // has to be able to find this wait; m_registration alone when it cannot.
    std::shared_ptr<detail::waiter> m_waiter;
    reactor::token                  m_registration = reactor::token::none;
};

/** Readable, which is almost every wait. */
inline until_ready readable(reactor& r, int fd, cancel_token t = cancel_token()) {
    return until_ready(r, fd, reactor::READ, t);
}

/** Writable, for a flush that filled the socket buffer. */
inline until_ready writable(reactor& r, int fd, cancel_token t = cancel_token()) {
    return until_ready(r, fd, reactor::WRITE, t);
}

/**
 * Suspend for a while.
 *
 * @throws cancelled if the token was set
 */
class until_after {
public:
    until_after(reactor& r, std::chrono::nanoseconds d,
                cancel_token t = cancel_token())
        : m_reactor(r), m_delay(d), m_token(t) {}

    bool await_ready() const { return m_token.requested(); }

    void await_suspend(std::coroutine_handle<> h) {
        m_reactor.after(m_delay, [h](reactor::timer_token) { h.resume(); });
    }

    void await_resume() const {
        if(m_token.requested()) throw cancelled();
    }

private:
    reactor&                 m_reactor;
    std::chrono::nanoseconds m_delay;
    cancel_token             m_token;
};

inline until_after sleep_for(reactor& r, std::chrono::nanoseconds d,
                             cancel_token t = cancel_token())
{
    return until_after(r, d, t);
}

/**
 * Move this coroutine onto a worker thread, and stay there.
 *
 * The other half of the design the reactor was built for: **I/O on the
 * reactor's thread, everything expensive on a pool.**  A handler that has read
 * a request and now has to parse, decompress, hash or otherwise burn CPU does
 * it here, so the reactor keeps dispatching for every other connection
 * meanwhile.
 *
 *     co_await sys::on_pool(c.pool());     // now on a worker
 *     ... the expensive part ...
 *     co_await sys::on_reactor(c.reactor());
 *
 * ## What is true on the far side
 *
 * **Not the reactor's thread**, so anything documented as reactor-thread-only
 * is out: add(), remove(), modify(), cancel(), a timer, cancel_token::request.
 *
 * Reading and writing still work, and that is deliberate rather than
 * accidental -- until_ready notices it is on the wrong thread and posts the
 * registration rather than making it.  The cost is that **an I/O await from a
 * worker resumes on the reactor's thread**, because that is where the
 * readiness callback runs.  So a read from the pool quietly hops back, which
 * is usually what was wanted and is never what was written.
 *
 * ## Two things that are unsafe across this
 *
 * A **std::mutex held across the hop** is unlocked by a different thread than
 * locked it, which is undefined.  Take it and release it on one side.
 *
 * **Thread-local state**, and OpenSSL's error queue in particular: clear it on
 * one thread and read it on another and the diagnostic describes whatever that
 * thread last did.  #204 made those diagnostics trustworthy; this is the way
 * to make them lie again.
 *
 * With no pool -- job_queue(0) -- post() runs the job inline, so this resumes
 * immediately on the same thread and the whole thing is a no-op.  A caller can
 * be written once and made serial or concurrent by one number, which is what
 * that mode is for.
 */
class on_pool {
public:
    explicit on_pool(job_queue& q) : m_queue(q) {}

    ~on_pool() {
        // Nothing to unregister -- a posted job is not a registration -- but
        // the pending resume has to be told the frame is gone.
        if(m_hop) m_hop->finished.store(true);
    }

    on_pool(const on_pool&) = delete;
    on_pool& operator=(const on_pool&) = delete;

    bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        m_hop = std::make_shared<detail::waiter>();

        m_hop->h = h;

        // Weak, so a job that runs after the frame was destroyed finds nothing
        // rather than resuming freed memory.  The same shape the cancellation
        // work needed, for the same reason.
        const std::weak_ptr<detail::waiter> w = m_hop;

        m_queue.post([w]{
            const std::shared_ptr<detail::waiter> s = w.lock();

            if(!s || s->finished.exchange(true)) return;

            s->h.resume();
        });
    }

    void await_resume() const {}

private:
    job_queue&                      m_queue;
    std::shared_ptr<detail::waiter> m_hop;
};

/**
 * Move this coroutine back onto the reactor's thread.
 *
 * The return leg of on_pool.  Uses reactor::post, which is documented safe
 * from any thread and is the only way another one may reach the reactor at
 * all.
 *
 * Awaiting it when already on the reactor's thread still costs a pass: the job
 * runs at the top of the next one rather than immediately, because a job that
 * ran inline could recurse into the dispatch it was posted from.
 */
class on_reactor {
public:
    explicit on_reactor(reactor& r) : m_reactor(r) {}

    ~on_reactor() { if(m_hop) m_hop->finished.store(true); }

    on_reactor(const on_reactor&) = delete;
    on_reactor& operator=(const on_reactor&) = delete;

    bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        m_hop = std::make_shared<detail::waiter>();

        m_hop->h = h;

        const std::weak_ptr<detail::waiter> w = m_hop;

        m_reactor.post([w]{
            const std::shared_ptr<detail::waiter> s = w.lock();

            if(!s || s->finished.exchange(true)) return;

            s->h.resume();
        });
    }

    void await_resume() const {}

private:
    reactor&                        m_reactor;
    std::shared_ptr<detail::waiter> m_hop;
};

/**
 * Cancel the token in d, unless the timer is cancelled first.
 *
 * The shape a deadline actually takes: not a parameter threaded through every
 * call, but a timer that cancels what is in flight.  #166 is this -- Pop3 and
 * Imap4 wait forever by default, and a wedged server hangs the caller -- and
 * the answer is one of these armed around the exchange.
 *
 * @return the timer, so a caller that finishes early can cancel it and stop
 *         the deadline firing into the *next* operation on the same token
 */
inline reactor::timer_token deadline(reactor& r, std::chrono::nanoseconds d,
                                     cancel_token t)
{
    return r.after(d, [t](reactor::timer_token) mutable { t.request(); });
}

/**
 * Run a task to completion, turning the reactor until it finishes.
 *
 * **The escape hatch that stops the colouring reaching main().**  A coroutine
 * can only be awaited by a coroutine, so without this every caller of an async
 * function would have to become one, all the way up.  With it, jlib-mail and
 * Imap4 keep their blocking shape and call this, while new code awaits
 * directly.
 *
 * **Not callable from a coroutine**, and not from a reactor callback: it turns
 * the reactor, and the reactor refuses a nested pass.
 *
 * @throws whatever the task threw, including cancelled
 */
template<typename T>
T run_until_complete(reactor& r, task<T>& t) {
    t.start();

    while(!t.done()) r.run_one();

    return t.result();
}

}
}

#endif // JLIB_SYS_AWAIT_HH
