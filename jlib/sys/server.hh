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

#ifndef JLIB_SYS_SERVER_HH
#define JLIB_SYS_SERVER_HH

#include <jlib/sys/listener.hh>
#include <jlib/sys/async_reader.hh>
#include <jlib/sys/async_writer.hh>
#include <jlib/sys/await.hh>
#include <jlib/sys/reactor.hh>
#include <jlib/sys/task.hh>
#include <jlib/sys/pipe.hh>
#include <jlib/sys/socketstream.hh>
#include <jlib/sys/sync.hh>
#include <jlib/sys/tls.hh>

#include <atomic>
#include <list>
#include <cstddef>
#include <exception>
#include <functional>
#include <string>

namespace jlib {
namespace sys {

/**
 * How a server is set up.  All of it is fixed at construction.
 *
 * At namespace scope rather than nested in server, and that is not style: a
 * default argument is a complete-class context, so `const policy& p = policy()`
 * on a member of the class that *nests* policy needs the nested aggregate's
 * member initializers while the enclosing class is still incomplete, and does
 * not compile.  An earlier version of this header routed around that by
 * splitting run() into two overloads.  Defining the struct first removes the
 * problem instead, and server::policy still spells.
 */
struct server_policy {
    /**
     * Handler threads.  Zero -- the default -- serves each connection on the
     * accept thread, one at a time.
     *
     * A serial server's liveness rests entirely on io_timeout: one client that
     * connects and says nothing blocks every other client for exactly as long
     * as a read may block.  Which is why io_timeout is not zero, and why a
     * caller who sets it to zero "to be safe" has built a denial of service out
     * of a single connection.
     */
    unsigned int threads = 0;

    /** Seconds a handler's reads and writes may block.  Zero is forever. */
    double io_timeout = 30;

    /**
     * How deep the job queue may get before the accept loop stops accepting.
     *
     * Queue depth and nothing else: a job that has been taken by a worker is
     * running, not queued, and does not count -- so a busy pool does not make
     * the queue look full and stall the accept loop against work already under
     * way.  Zero means "threads", resolved at construction.
     *
     * **This bounds descriptors, not concurrency.**  Concurrency is threads.
     * Since up to threads jobs can be running, the process holds at most
     * max_queued + threads connection descriptors.  Two numbers, two jobs, and
     * blurring them is how somebody later removes the cap as redundant and
     * finds the descriptor bound was load-bearing.
     */
    std::size_t max_queued = 0;

    /**
     * How many connections an *async* server will carry at once.
     *
     * The analogue of max_queued for a server with no queue.  A coroutine per
     * connection is cheap -- a frame, not a stack -- but it is not free, and a
     * server that accepts without limit is one a flood takes down.  Beyond
     * this the accept loop stops accepting and the overflow waits in the
     * kernel's listen backlog, which is where a connection is supposed to
     * wait.
     *
     * Ignored by a blocking server, which bounds itself with threads and
     * max_queued.
     */
    std::size_t max_connections = 256;
};

/**
 * Accept connections and hand each one to a handler as a stream.
 *
 * jlib was a client for twenty-six years.  listener gave it an accepting
 * socket; this gives it something to do with one.
 *
 * ## Deliberately small, and blocking or not by which handler it was given
 *
 * It accepts, optionally secures, and dispatches.  There is no connection
 * reuse and no protocol.
 *
 * **Two modes, chosen at construction.**  Given a `handler`, a connection goes
 * to a job_queue and a thread blocks on it until it is done -- one connection
 * per job, which is what this was for twenty-six years of being a client and
 * then a small server.  Given an `async_handler`, the connection never leaves
 * the reactor: it is a coroutine suspended at whatever read or write it is
 * waiting on, `policy::threads` is not used at all, and `max_connections`
 * bounds it instead.
 *
 * The blocking mode is not deprecated and is still the right thing for a
 * handler that wants a std::iostream.  What the async one buys is that a
 * connection costs a coroutine frame rather than a thread, and that a handler
 * can be given a deadline -- see connection::token().
 *
 * The thing that had to survive that change is the *handler contract* -- given
 * a connection, do something with it -- which is why run() is a thin loop over
 * serve_one() and not the other way round.  It did: sys_server_test passes
 * unaltered.
 *
 * It is a server for a loopback OAuth2 redirect and a test harness.  **It is
 * not hardened for a public port**: nothing here defends against a slow-loris,
 * a flood, or a client that connects and never speaks, beyond a thread count, a
 * queue depth, a listen backlog and a read timeout.  Saying so is the only
 * thing that keeps a narrow thing narrow.
 *
 * ## One reference covers both transports
 *
 * A handler takes a socketstream&, and gets a TLS connection through the same
 * reference, because basic_tlsstream derives from basic_socketstream.  A
 * handler cannot tell which it was given, which is the point -- net/http.cc
 * already holds a unique_ptr<socketstream> for both and this is that idiom
 * pointed the other way.
 *
 * ## Where the connections wait
 *
 * A job_queue holds what has been accepted and not yet started, so this does
 * accept a little ahead of its capacity -- which an earlier version did not,
 * and which completes a TCP handshake for a client that cannot be served yet.
 * policy::max_queued is what keeps that bounded: the accept loop waits for room
 * *before* it polls, so the overflow stays in the kernel's listen backlog,
 * where a connection is supposed to wait and where the backlog refuses it with
 * behaviour every client understands.
 */
class server {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = "server exception: " + msg;
        }
        virtual ~exception() {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }
    protected:
        std::string m_msg;
    };

    using policy = server_policy;

    /**
     * What runs for one accepted connection.
     *
     * **The reference is valid only for the duration of the call.**  The stream
     * is destroyed, and its descriptor closed, the moment the handler returns;
     * a handler that stores the reference has stored a dangling one.
     */
    typedef std::function<void(socketstream&, const peer&)> handler;

    /**
     * A connection, as an async handler sees it.
     *
     * A reader and a writer rather than a stream, because a coroutine cannot
     * suspend inside operator>>.  The same pair every converted framing
     * function already takes -- util::http::read_head and imap::read were
     * written against async_reader& and work here untouched.
     *
     * **Plain or TLS, and a handler cannot tell**, which is the property the
     * blocking contract has through socketstream& and which is worth keeping:
     * async_tls hands out the same two interfaces async_fd_reader and
     * async_fd_writer implement.
     */
    class connection {
    public:
        connection(sys::reactor& r, async_reader& rd, async_writer& w,
                   const peer& from, cancel_token t)
            : m_reactor(r), m_reader(rd), m_writer(w), m_peer(from),
              m_token(t) {}

        async_reader& reader() { return m_reader; }
        async_writer& writer() { return m_writer; }

        /**
         * The loop this connection is running on.
         *
         * A handler needs it to do anything async beyond reading and writing
         * -- and the first of those is a deadline:
         *
         *     sys::deadline(c.reactor(), std::chrono::seconds(10), c.token());
         *
         * which is what bounds a client that connects and then says nothing.
         * Without this a handler could suspend but not arm anything, which
         * makes the token below unusable.
         */
        sys::reactor& reactor() { return m_reactor; }

        const peer& from() const { return m_peer; }

        /**
         * This connection's own token.
         *
         * Cancelled when the server stops, and available to a handler that
         * wants a deadline of its own -- sys::deadline(reactor, d, token)
         * around a read is what bounds a client that connects and says
         * nothing.
         */
        cancel_token& token() { return m_token; }

    private:
        sys::reactor& m_reactor;
        async_reader& m_reader;
        async_writer& m_writer;
        peer          m_peer;
        cancel_token  m_token;
    };

    /**
     * A handler that suspends rather than blocking.
     *
     * **A connection served this way does not occupy a thread.**  It lives on
     * the server's reactor as a coroutine, suspended at whatever read or write
     * it is waiting on, and the accept loop carries on.  policy::threads and
     * the job queue are not used at all -- there is nothing to dispatch.
     *
     * The blocking handler is unchanged and is still the right thing for a
     * handler that wants a std::iostream.  One or the other, chosen by which
     * constructor was called.
     */
    typedef std::function<task<void>(connection&, const peer&)> async_handler;

    /**
     * What to do with an exception a handler let escape, or a handshake that
     * did not complete.  Runs on whichever thread the connection did.
     *
     * The default writes one line to std::cerr, assembled whole before it is
     * written -- several threads reporting at once otherwise interleave in the
     * middle of a message.
     */
    typedef std::function<void(const std::exception&, const peer&)> error_handler;

    /** From a listener already bound: the caller chose the port and backlog. */
    server(listener l, handler h, tls_context tls = tls_context(),
           const policy& p = policy());

    /**
     * The same, with a handler that suspends.
     *
     * policy::threads and policy::max_queued are ignored: there is no queue
     * and no pool, because a connection never leaves the reactor.  What bounds
     * concurrency instead is policy::max_connections.
     */
    server(listener l, async_handler h, tls_context tls = tls_context(),
           const policy& p = policy());

    server(unsigned short port, async_handler h,
           const std::string& host = "127.0.0.1",
           tls_context tls = tls_context(),
           const policy& p = policy());

    /** Bind and serve.  Loopback by default, for the reason listener gives. */
    server(unsigned short port, handler h,
           const std::string& host = "127.0.0.1",
           tls_context tls = tls_context(),
           const policy& p = policy());

    /**
     * stop(), then join().
     *
     * It has to be both.  A handler reaches this object through the handler it
     * was given, so the wait is what keeps that reference valid.
     *
     * **This can block**, for as long as the queued connections take to serve:
     * the drain is what stops something already accepted being silently
     * discarded, and io_timeout is what keeps it finite.  A handler that blocks
     * forever hangs this, which is why io_timeout does not default to never.
     */
    ~server();

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    unsigned short port() const;
    bool tls() const;

    /** Fixed at construction.  There is no setter; see server_policy. */
    const policy& get_policy() const { return m_policy; }

    /** Accepted connections waiting for a worker.  Always 0 when serial. */
    std::size_t pending() const;

    void on_error(error_handler h);

    /**
     * Accept at most one connection and serve it.
     *
     * @param timeout seconds to wait; zero waits until a connection arrives or
     *                stop() is called.  The wait covers admission control as
     *                well as the accept: with the queue full this spends the
     *                timeout waiting for room and returns false without
     *                accepting, leaving the connection in the listen backlog.
     * @return true if a connection was accepted, false if the wait ran out or
     *         stop() interrupted it.
     *
     * True means accepted, not served: with a pool the handler has been posted
     * and may not have started.
     *
     * A failure inside one connection -- a handshake that did not complete, a
     * handler that threw -- goes to on_error() and is not an error here.  A
     * failure of the listener itself throws.
     *
     * **One accept thread.**  Nothing here calls this concurrently, and the
     * depth cap is only exact if nothing does: two threads can each find room
     * and then both post.
     */
    bool serve_one(double timeout = 0);

    /**
     * serve_one() until stop().  Blocking; run it on a thread if you want one.
     *
     * **One-shot.**  A stopped server cannot be restarted, because
     * job_queue::stop() retires the pool and nothing respawns it -- a restarted
     * server would accept connections and drop every one in silence.  So unlike
     * an earlier version this does not clear the stop flag on entry, and
     * calling it after stop() returns immediately.
     */
    void run();

    /**
     * Ask run(), and any serve_one() that is waiting, to return.
     *
     * Safe from any thread, including from inside a handler.  Calling it twice
     * is harmless, but it is *not* first-wins: see @p drain.
     *
     * **This is not a barrier**, and everywhere else in this library "stop"
     * means stopped.  Handlers already running keep running, and whatever they
     * captured is still in use when this returns.  join() waits for them; the
     * destructor calls both.
     *
     * It has to wake all three ways a thread can be blocked here -- the flag,
     * the queue's depth wait, and poll(2) -- and the last is a byte down a pipe
     * rather than closing the listening descriptor, because closing one that
     * another thread is blocked in poll(2) on is undefined and on macOS does
     * not wake it.
     *
     * @param drain  serve what has been accepted but not yet started, or throw
     *               it away.
     *
     * Queued work only.  A handler already running is unaffected either way, so
     * this draws the same line between waiting and running that max_queued
     * draws against threads -- and for the same reason, since a job that has
     * been popped is no longer the queue's to abandon.
     *
     * False drops those connections with no answer at all.  Their descriptors
     * are closed rather than leaked, because each rides in the job's
     * shared_ptr<held_fd> and dropping the job destroys it, so the client sees
     * an immediate close rather than waiting out its own timeout -- but it is
     * still a close in place of a response, which is why true is the default.
     *
     * job_queue::stop() assigns its drain flag every time, so a later
     * stop(true) silently upgrades an earlier stop(false) back to draining.
     * That is why join() does not stop on your behalf: it would have to guess
     * at this, and guessing overwrites what the caller asked for.
     */
    void stop(bool drain = true);

    bool stopped() const;

    /**
     * Wait for the handler threads to retire.  **stop() first.**
     *
     * A job_queue's workers wait for work forever, so "wait for them to finish"
     * is only a terminating question once they have been told to leave.  This
     * does not tell them.  An earlier version called stop() here to make the
     * ordering impossible to get wrong; it no longer does, because stop() is
     * not first-wins and joining would have to guess at its drain argument.
     *
     * So the sequence is stop() then join(), which is what the destructor does.
     * Serving is over when this returns.  It is not a barrier you can take in
     * the middle of a run and carry on from.
     *
     * **A join() without a stop() hangs** -- and hangs only with a pool.  With
     * threads == 0 there is nothing to retire and this returns at once, so a
     * serial server survives the wrong order and a pooled one deadlocks on it.
     * That is a bad failure mode to discover by adding a thread later, so the
     * order is worth getting right even where it currently cannot bite.
     *
     * **Never from a handler**: with a pool that joins a worker to itself.  And
     * it does not wait for run() -- join the thread you started run() on
     * yourself, before this object goes out of scope.
     */
    void join();

private:
    /**
     * Tag for the constructor the async one delegates to.
     *
     * Which exists so the blocking constructor can keep throwing on a null
     * handler.  Delegating to it with an empty one and installing the async
     * handler afterwards would have meant moving that check somewhere both
     * paths reach -- and the only such place is serve_one, which turns a
     * construction-time error into a first-use one.  A server built wrong
     * should say so when it is built.
     */
    struct deferred_handler_t { explicit deferred_handler_t() = default; };

    server(deferred_handler_t, listener l, tls_context tls, const policy& p);

    void serve(int fd, const peer& from);
    std::size_t cap() const;

    /** One accept and one post.  What the reactor calls the listener ready for. */
    bool accept_and_post();

    /** Whether an async server is carrying all the connections it will. */
    bool full() const;

    /** The async path: one accept, and a coroutine that owns the descriptor. */
    bool accept_and_start();

    /** The coroutine one connection runs in. */
    task<void> serve_async(int fd, peer from, cancel_token t);

    /** Drop the connections that have finished.  Called once per pass. */
    void reap();

    listener      m_listener;
    handler       m_handler;
    error_handler m_on_error;
    tls_context   m_tls;
    policy        m_policy;

    // Declared after m_listener, so it is destroyed *before* it: the
    // registration below names a descriptor the listener owns.
    //
    // The wake pipe used to live here, alongside a two-element pollfd array
    // rebuilt on every call.  Both moved into the reactor, and with them the
    // note about why a byte down a pipe beats closing the listening descriptor
    // -- which is undefined while another thread polls it and on macOS does not
    // wake it.
    reactor        m_reactor;
    reactor::token m_listen = reactor::token::none;

    // This pass's answer, written by the callback and read by serve_one.  A
    // plain bool because only the reactor's thread touches it, which is the
    // thread that called serve_one.
    bool m_accepted = false;

    // What the callback threw, carried back out.  A reactor callback's
    // exception goes to on_error and the pass continues, but serve_one has
    // always let an accept failure propagate to its caller -- so it is caught,
    // parked here, and rethrown once the pass is over.
    std::exception_ptr m_accept_error;

    async_handler m_async;

    /**
     * The connections in flight, each a suspended coroutine.
     *
     * Held because a coroutine started and forgotten is a leak: something has
     * to own the frame until it completes.  Each carries its own token so that
     * stop() can end a connection that is parked on a peer which will never
     * speak -- which before #212 was not something a token could do.
     */
    struct live {
        task<void>   work;
        cancel_token token;
    };

    std::list<live> m_live;

    // Whether the listener's registration is currently disabled because the
    // connection cap is reached.  Disabling it rather than accepting and
    // dropping is what keeps the overflow in the kernel's listen backlog,
    // where a connection is supposed to wait -- and what stops the accept loop
    // spinning on a readiness it refuses to act on.
    bool m_listen_off = false;

    std::atomic<bool> m_stop{false};

    // Declared last, so it is destroyed first: ~job_queue is stop() then
    // join(), and every worker has to have left before m_handler, m_tls and
    // m_listener are destroyed -- a running handler reaches all three.  The
    // destructor below makes that redundant in the ordinary case and not
    // redundant when a constructor throws after the pool is built.
    job_queue m_jobs;
};

}
}

#endif // JLIB_SYS_SERVER_HH
