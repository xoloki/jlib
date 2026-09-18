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
#include <map>
#include <mutex>
#include <utility>
#include <cstddef>
#include <exception>
#include <functional>
#include <string>
#include <vector>

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

    /**
     * How long a client has.  Zero is forever.
     *
     * **It means two different things, and the difference is the point.**
     *
     * On a *blocking* server it is `SO_RCVTIMEO`/`SO_SNDTIMEO` on the
     * connection: a bound on each read and each write, applied by the kernel.
     * Per operation, so it resets every time -- and a client that sends one
     * octet every twenty-nine seconds never trips it and holds a connection
     * for as long as it cares to.  That is a slow-loris, and this cannot stop
     * one.
     *
     * On an *async* server there is no socket timeout; a handler that wants a
     * bound arms a deadline on connection::token(), and what it bounds is up
     * to it.  net::http::server uses this value for one over the whole request
     * read -- cumulative, not per operation -- so the same client is dropped.
     *
     * Same number, same intent, and the async reading is the stricter of the
     * two.  A caller porting a handler between them should know which it is
     * getting.
     */
    double io_timeout = 30;

    /**
     * How deep the job queue may get before the accept loop stops accepting.
     *
     * Queue depth and nothing else: a job that has been taken by a worker is
     * running, not queued, and does not count -- so a busy pool does not make
     * the queue look full and stall the accept loop against work already under
     * way.  Zero means "threads", resolved at construction.
     *
     * **On an async server this is the *pool*, not the connections.**  A
     * connection there is a coroutine on the reactor and never occupies a
     * thread; what these serve is `co_await sys::on_pool(c.pool())` -- the
     * parts of a handler that are computation rather than I/O.  Zero means the
     * hop runs inline and the handler is serial, which is job_queue's usual
     * bargain.
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

    /**
     * How many connections one address may hold at once.  Zero is no limit,
     * and no limit is the default.
     *
     * `max_connections` bounds the server; this bounds each client. Without
     * it, one address holds every slot the server has and everybody else waits
     * in the backlog behind it -- which is the difference between a server and
     * a target.
     *
     * **Enforced by accepting and closing**, which is the opposite of what
     * `max_connections` does and is not a choice. That one disarms the
     * listener so the overflow waits in the kernel's backlog; this one cannot,
     * because there is no way to know who is calling without accepting the
     * call. The cost is one accept and one close per refused connection, paid
     * by the address doing the flooding.
     *
     * The refusal is silent: no bytes, no status. There is no protocol here to
     * say anything in -- this layer does not know it is speaking HTTP -- and a
     * server that answered would be doing work on behalf of the flood.
     * `net::http::server::rate_limit()` is where a refusal that explains
     * itself lives.
     *
     * Applies to both servers. A blocking one bounds its *concurrency* with
     * threads and max_queued, but that is a bound on the server again rather
     * than on one client: a single address can fill the queue and every other
     * client waits behind it.
     *
     * The address is the peer and nothing else. See the note on
     * `net::http::server::rate_limit()` about why no forwarded header is
     * believed, and what that means behind a proxy.
     */
    std::size_t max_per_address = 0;
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
 * not hardened for a public port**: what it has against a slow-loris, a flood,
 * or a client that connects and never speaks is a thread count, a queue depth,
 * a listen backlog, a read timeout and max_connections -- and, on the async
 * side, a cancel_token per connection, which is what makes a deadline over a
 * whole read possible at all.  net::http::server arms one; this does not arm
 * it for you.
 *
 * That list has grown and the sentence above has not changed, which is the
 * point of it.  Saying so is the only thing that keeps a narrow thing narrow,
 * and net::http::server -- which started here and now routes, caches and
 * serves a directory -- is the worked example of how fast that stops being
 * automatic.
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
    /**
     * How many connections each address is holding.
     *
     * **Bounded by construction, which is the difference from the rate
     * limiter's table.** An entry exists only while a connection does, and
     * goes when the count reaches zero -- so the table cannot be larger than
     * the connections the server is already carrying, and those are bounded by
     * policy. There is nothing to sweep and no way for an address flood to
     * grow it: a flood that is refused was never counted, and one that is
     * admitted is a connection like any other.
     *
     * Public so that per-address separation can be tested at all. It cannot be
     * reached through a server -- every client in this tree connects over
     * loopback and both platforms report the peer as 127.0.0.1 whichever
     * loopback address is dialled, measured rather than assumed.
     */
    class address_count {
    public:
        /**
         * One counted connection, released when this is destroyed.
         *
         * RAII rather than a matching call, because the decrement has to
         * happen on every path a connection can end on -- returned, thrown
         * out of, cancelled, or the job dropped before it ran -- and a count
         * that misses one drifts upward and refuses that address forever.
         *
         * Move-only. Two holds for one connection is exactly the
         * double-decrement this exists to prevent.
         */
        class hold {
        public:
            hold() : m_owner(0) {}
            ~hold();

            hold(hold&& o) : m_owner(o.m_owner), m_who(std::move(o.m_who)) {
                o.m_owner = 0;
            }

            hold& operator=(hold&& o);

            hold(const hold&) = delete;
            hold& operator=(const hold&) = delete;

            /** Whether this counts anything. */
            bool engaged() const { return m_owner != 0; }

        private:
            friend class address_count;

            hold(address_count& c, const std::string& who)
                : m_owner(&c), m_who(who) {}

            address_count* m_owner;
            std::string    m_who;
        };

        /**
         * Count one more connection from `who`, unless that would exceed
         * `cap`.
         *
         * The test and the increment are one operation under the lock. They
         * are serialised anyway today -- both accept loops are
         * single-threaded -- but the *releases* are not, and a cap that was
         * only accidentally atomic is one that breaks the first time
         * somebody accepts on two threads.
         *
         * @param cap  0 for no limit, in which case this always succeeds
         * @return     engaged when the connection may proceed
         */
        hold take(const std::string& who, std::size_t cap);

        /** How many connections `who` is holding right now. */
        std::size_t count(const std::string& who) const;

        /** How many addresses are being counted. */
        std::size_t tracked() const;

    private:
        friend class hold;

        void release(const std::string& who);

        mutable std::mutex                 m_lock;
        std::map<std::string, std::size_t> m_counts;
    };

    /** How many connections `address` is holding.  See policy::max_per_address. */
    std::size_t connections_from(const std::string& address) const;

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
        connection(sys::reactor& r, job_queue& q, async_reader& rd,
                   async_writer& w, const peer& from, cancel_token t,
                   int fd = -1)
            : m_reactor(r), m_pool(q), m_reader(rd), m_writer(w),
              m_peer(from), m_token(t), m_fd(fd) {}

        /**
         * Has the peer hung up?  Asked without writing anything.
         *
         * A handler that streams learns the client left from a failed write --
         * `responder::live()`, `relay::wanted()` -- which makes that knowledge
         * **only as fresh as the last write.** A handler that produces one
         * whole answer after a minute of work never writes until it is
         * finished, so it never finds out at all, and spends the minute on a
         * reply nobody will read. That is jserve's non-streaming path.
         *
         * `recv(MSG_PEEK)` rather than polling for readability, and the
         * difference matters: on a kept-alive connection readable may be a
         * **pipelined next request**, which is not a peer that left. A peek
         * distinguishes them -- 0 means end of stream -- and does not consume,
         * so anything it finds is still there for the reader that wants it.
         *
         * ## Which threads may call it
         *
         * **Any thread whose lifetime is bounded by the connection's.** The
         * peek itself is safe from anywhere -- it takes nothing from the
         * reactor, because it does not consume -- so the only hazard is the
         * descriptor being closed and its number reused underneath the call.
         *
         * `sys::relay` gives that bound: it joins in its destructor, so a
         * generation thread cannot outlive the frame that owns it, and that
         * frame is destroyed before this connection's descriptor is closed.
         * A thread that is *not* joined before teardown must not call this.
         *
         * ## What false means
         *
         * "Not known to have gone", not "still there". It answers false for a
         * TLS connection whatever the peer has done, because the descriptor
         * carries ciphertext: bytes waiting are not a request and no bytes are
         * not a live peer, and the real answer needs the TLS layer's view of
         * close_notify. That is the right direction to be wrong in -- a model
         * held a little longer costs time, a reply abandoned on a guess costs
         * the answer.
         */
        bool peer_gone() const;

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

        /**
         * The worker pool, for the parts that are not I/O.
         *
         *     co_await sys::on_pool(c.pool());
         *     ... parse, decompress, hash ...
         *     co_await sys::on_reactor(c.reactor());
         *
         * **This is what policy::threads means on an async server.**  It is
         * not how many connections are served at once -- that is
         * max_connections, and the reactor serves them all on one thread --
         * it is how many can be doing something expensive at once.
         *
         * With threads == 0 the queue runs a job on the thread that posted it,
         * so the hop is a no-op and a handler written this way is simply
         * serial.  One number, two behaviours, which is what job_queue's zero
         * mode has always been for.
         */
        job_queue& pool() { return m_pool; }

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
        job_queue&    m_pool;
        async_reader& m_reader;
        async_writer& m_writer;
        peer          m_peer;

        // -1 when there is no descriptor this can honestly peek: a TLS
        // connection has one, and peeking it would read ciphertext.
        int           m_fd = -1;
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

    /**
     * One bound port and the identity it answers with.
     *
     * An empty context is a plaintext port. Two of these with the *same*
     * context share its SNI table, since a tls_context is a refcount over one
     * SSL_CTX -- which is what makes "the same certificate on 443 and 8443"
     * free rather than something to configure twice.
     */
    struct bound {
        listener    l;
        tls_context tls;

        bound(listener l_, tls_context tls_ = tls_context())
            : l(std::move(l_)), tls(std::move(tls_)) {}
    };

    /**
     * Several ports at once, each with its own identity.
     *
     * What one server buys over several is everything above the listener: one
     * reactor, one pool, one connection cap, one address table, and one
     * handler. Two servers would have two of each, which for 80-and-443 is two
     * reactor threads and a cap that an attacker gets to spend twice.
     *
     * The list must not be empty, and every listener must already be bound.
     */
    server(std::vector<bound> ports, handler h, const policy& p = policy());

    /** The same, with a handler that suspends. */
    server(std::vector<bound> ports, async_handler h,
           const policy& p = policy());

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

    /**
     * The first listener's port, and whether *that* listener is TLS.
     *
     * They answer about the same listener on purpose. Reading tls() as "does
     * this server speak TLS anywhere" would be the friendlier sentence and the
     * wrong one: a caller doing `if(s.tls()) connect_tls(s.port())` would then
     * be dialling a plaintext port over TLS, and it would be right about both
     * halves separately.
     *
     * For a server with one port -- which is nearly all of them, and every
     * test in this tree -- these are what they always were.
     */
    unsigned short port() const;
    bool tls() const;

    /** Every port, in the order they were given. */
    std::vector<unsigned short> ports() const;

    /** How many listeners there are; port(i) and tls(i) index them. */
    std::size_t listeners() const;

    unsigned short port(std::size_t i) const;
    bool tls(std::size_t i) const;

    /** Fixed at construction.  There is no setter; see server_policy. */
    const policy& get_policy() const { return m_policy; }

    /** Accepted connections waiting for a worker.  Always 0 when serial. */
    std::size_t pending() const;

    void on_error(error_handler h);

    /**
     * Hand something to the error handler that this did not catch itself.
     *
     * A handler that throws already reaches `on_error`. A request this server
     * *refused* did not, and that was a gap the #240 audit found rather than a
     * design: a smuggling attempt, an unparseable head, a target with a
     * control character in it -- every one of them answered 400 and vanished.
     * An operator had no way to see an attack in progress, because the only
     * account of it went to the attacker in the response body.
     *
     * Always safe to call: there is a default handler and it is never null.
     */
    void report(const std::exception& e, const peer& from) const;

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
     *
     * **Asynchronous.  join() is the synchronous half.**  On an async server
     * this returns before the live connections have been ended: ending one
     * runs its own code and destroys its frame, which only the reactor's
     * thread may do, so it is posted there.  run() returns once that job has
     * run, which makes the thread turning the reactor the thing a caller waits
     * on -- as it already was.
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

    server(deferred_handler_t, std::vector<bound> ports, const policy& p);

    // The tls_context is passed by value, not by reference, and that is
    // load-bearing on the async side: serve_async is a coroutine, and a
    // coroutine's parameters are copied into the frame *as declared*, so a
    // reference parameter copies the reference and dangles the moment the
    // caller's frame goes.  A copy is a refcount bump on one SSL_CTX.
    void serve(int fd, const peer& from, tls_context tls,
               address_count::hold slot);
    std::size_t cap() const;

    /** One accept and one post.  What the reactor calls listener `i` ready for. */
    bool accept_and_post(std::size_t i);

    /**
     * Take a slot for `from` if policy allows, closing `fd` if it does not.
     *
     * @return engaged when the connection may proceed
     */
    bool admit(int fd, const peer& from, address_count::hold& into);

    /** Whether an async server is carrying all the connections it will. */
    bool full() const;

    /** The async path: one accept, and a coroutine that owns the descriptor. */
    bool accept_and_start(std::size_t i);

    /** The coroutine one connection runs in. */
    task<void> serve_async(int fd, peer from, tls_context tls, cancel_token t,
                           address_count::hold slot);

    /** Drop the connections that have finished.  Called once per pass. */
    void reap();

    /**
     * End every live connection.  **Reactor thread only.**
     *
     * Requesting a token resumes the coroutine parked on it, and it resumes
     * *here* -- so this runs a connection to its next suspension, or to its
     * end, on whichever thread calls it.  That thread has to be the reactor's,
     * because a connection that ends destroys its frame, closes its descriptor
     * and leaves m_live, all of which the reactor thread is also doing.
     */
    void cancel_live();

    address_count m_addresses;

    /**
     * The ports this serves, each with its own identity.
     *
     * **TLS belongs to the listener, not to the server**, which is the whole
     * of what made one port a limit: a server holding one context can describe
     * every connection as encrypted or none of them, and 80-and-443 is neither.
     *
     * Built once, in the constructor, and never resized -- the callbacks
     * registered below hold an index into it, and a vector that grew would
     * move the elements those indices name.  There is deliberately no
     * add_listener(): every port is known before the reactor turns, and
     * registering a descriptor while the loop is running is a question nobody
     * has had to ask yet.
     */
    /**
     * What the server keeps: a bound port, its identity, and its registration.
     *
     * Separate from the public `bound` because the token is the server's
     * business -- a caller describing a port it wants served has no use for a
     * reactor handle and should not have to look at one.
     */
    struct listening {
        listener       l;
        tls_context    tls;
        reactor::token token = reactor::token::none;

        listening(bound b) : l(std::move(b.l)), tls(std::move(b.tls)) {}
    };

    std::vector<listening> m_bound;

    handler       m_handler;
    mutable error_handler m_on_error;
    policy        m_policy;

    // Declared after m_bound, so it is destroyed *before* it: the
    // registrations below name descriptors the listeners own.
    //
    // The wake pipe used to live here, alongside a two-element pollfd array
    // rebuilt on every call.  Both moved into the reactor, and with them the
    // note about why a byte down a pipe beats closing the listening descriptor
    // -- which is undefined while another thread polls it and on macOS does not
    // wake it.
    reactor        m_reactor;

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
    //
    // One flag for all of them, because the cap is one number: full() counts
    // connections, not connections-per-port, so a server at its limit stops
    // accepting everywhere.  Per-listener flags would let an attacker have the
    // cap twice by knocking on two doors.
    bool m_listen_off = false;

    std::atomic<bool> m_stop{false};

    // Declared last, so it is destroyed first: ~job_queue is stop() then
    // join(), and every worker has to have left before m_handler and m_bound
    // are destroyed -- a running handler reaches both, including the
    // tls_context its connection was wrapped with.  The
    // destructor below makes that redundant in the ordinary case and not
    // redundant when a constructor throws after the pool is built.
    job_queue m_jobs;
};

}
}

#endif // JLIB_SYS_SERVER_HH
