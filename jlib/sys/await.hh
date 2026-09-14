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
#include <jlib/sys/task.hh>

#include <chrono>
#include <exception>
#include <memory>
#include <string>

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

/**
 * A shared "stop what you are doing" flag.
 *
 * **Nothing in jlib cancels anything yet**, and this is deliberately shaped
 * rather than used.  The reason is written down in #4: server.hh says it does
 * not defend against "a slow-loris, a flood, or a client that connects and
 * never speaks", and every one of those defences is *"this connection has had
 * long enough, tear it down"* -- which needs an operation parked in a
 * co_await to be resumable with an error rather than only with success.
 *
 * Retrofitting that means touching every await site, because a call that could
 * not fail suddenly can.  Carrying the token now costs a pointer and a branch.
 * The same reasoning left m_nonblocking in the TLS classifier in #204.
 *
 * ## Not thread safe, on purpose
 *
 * request() may be called from the reactor's thread -- from a timer, or from a
 * callback -- and nowhere else.  A token a second thread could set would need
 * the flag to be atomic *and* the wake to be ordered against the reactor's
 * dispatch, which is a bigger promise than anything needs yet.  Another thread
 * cancels by post()ing to the reactor, which is how another thread reaches
 * everything else here.
 */
class cancel_token {
public:
    cancel_token() : m_state(new state) {}

    /** Ask.  Anything suspended on this token throws cancelled when resumed. */
    void request() { m_state->requested = true; }

    bool requested() const { return m_state->requested; }

    /**
     * Shares the flag, not a copy of it.
     *
     * So a token handed to a nested operation is the same token -- cancelling
     * the outer one cancels everything below it, which is what a connection
     * teardown means.
     */
    cancel_token(const cancel_token&) = default;
    cancel_token& operator=(const cancel_token&) = default;

private:
    struct state { bool requested = false; };

    std::shared_ptr<state> m_state;
};

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
     * Harmless in the ordinary case: once() removes the registration before
     * the callback runs, and reactor::remove() on a token that is already gone
     * is documented as a no-op rather than an error.
     *
     * **Must run on the reactor's thread**, because remove() may only be
     * called from there.  Destroying a task from another thread was never
     * sound -- the frame may be mid-resumption -- and this does not make it
     * so.
     */
    ~until_ready() {
        if(m_registration != reactor::token::none)
            m_reactor.remove(m_registration);
    }

    // A user-declared destructor suppresses the implicit moves.  Neither is
    // wanted: this is constructed in place by readable()/writable() and bound
    // straight to a co_await, so guaranteed elision covers every use.
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
        // Kept, so the destructor can unregister if this frame is destroyed
        // before the descriptor ever becomes ready.
        m_registration =
            m_reactor.once(m_fd, m_events,
                           [this, h](reactor::token, int, reactor::event_type) {
                               // Cleared first: once() has already removed the
                               // registration by the time a callback runs, and
                               // resume() may destroy this object.
                               m_registration = reactor::token::none;

                               h.resume();
                           });
    }

    /**
     * @throws cancelled if the token was set
     *
     * The one line that makes every future caller cancellation-aware.  A
     * `void await_resume() noexcept {}` here would be simpler today and a
     * refactor across every await site later.
     */
    void await_resume() const {
        if(m_token.requested()) throw cancelled();
    }

private:
    reactor&            m_reactor;
    int                 m_fd;
    reactor::event_type m_events;
    cancel_token        m_token;

    // none unless suspended and not yet resumed.
    reactor::token      m_registration = reactor::token::none;
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
 * Run a task to completion, turning the reactor until it finishes.
 *
 * **The escape hatch that stops the colouring reaching main().**  A coroutine
 * can only be awaited by a coroutine, so without this every caller of an async
 * function would have to become one, all the way up.  With it, jlib-mail and
 * Imap4 keep their blocking shape and call this, while new code awaits
 * directly -- which is the hybrid #4 settles on.
 *
 * **Not callable from a coroutine**, and not from a reactor callback: it turns
 * the reactor, and the reactor refuses a nested pass.  A coroutine that wants
 * another task's value co_awaits it.
 *
 * @throws whatever the task threw, including cancelled
 */
template<typename T>
T run_until_complete(reactor& r, task<T>& t) {
    t.start();

    // forever, not a timeout: the task finishing is the exit condition, and a
    // caller that wants a deadline arms a timer that cancels the token.
    while(!t.done()) r.run_one();

    return t.result();
}

}
}

#endif // JLIB_SYS_AWAIT_HH
