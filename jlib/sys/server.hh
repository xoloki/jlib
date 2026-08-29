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
#include <jlib/sys/pipe.hh>
#include <jlib/sys/socketstream.hh>
#include <jlib/sys/sync.hh>
#include <jlib/sys/tls.hh>

#include <atomic>
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
};

/**
 * Accept connections and hand each one to a handler as a stream.
 *
 * jlib was a client for twenty-six years.  listener gave it an accepting
 * socket; this gives it something to do with one.
 *
 * ## Deliberately small, and deliberately blocking
 *
 * It accepts, optionally secures, and dispatches.  There is no event loop, no
 * connection reuse, no protocol.  An event-driven design belongs with the async
 * I/O work, and the thing that has to survive that change is the *handler
 * contract* -- given a connection, do something with it -- which is why run()
 * is a thin loop over serve_one() and not the other way round.
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
     * Safe from any thread, including from inside a handler.  Idempotent.
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
     */
    void stop();

    bool stopped() const;

    /**
     * Stop taking work, and wait for what has been accepted to be served.
     *
     * **This stops the queue**, and that is not an implementation detail to be
     * tidied away: a job_queue's workers wait for work forever, so "wait for
     * them to finish" is only a terminating question once they have been told
     * to leave.  A join() that did not stop would hang, which is exactly what
     * an earlier version of this did.
     *
     * So: serving is over when this returns.  It is the second half of the
     * destructor, callable on its own -- not a barrier you can take in the
     * middle of a run and carry on from.
     *
     * **Never from a handler**: with a pool that joins a worker to itself.  And
     * it does not wait for run() -- join the thread you started run() on
     * yourself, before this object goes out of scope.
     */
    void join();

private:
    void serve(int fd, const peer& from);
    std::size_t cap() const;

    listener      m_listener;
    handler       m_handler;
    error_handler m_on_error;
    tls_context   m_tls;
    policy        m_policy;

    // Non-blocking at both ends, so a stop() called a thousand times cannot
    // block once the pipe fills.
    pipe m_wake{false, false};

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
