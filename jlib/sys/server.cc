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

#include <jlib/sys/server.hh>

#include <jlib/sys/async_tls.hh>

#include <jlib/sys/sslstream.hh>

#include <openssl/err.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

#include <unistd.h>

namespace jlib {
namespace sys {

namespace {

    void complain(const std::exception& e, const peer& from) {
        std::ostringstream o;

        o << "jlib::sys::server: " << (from.address.empty() ? "a client"
                                                            : from.address)
          << ": " << e.what() << "\n";

        // One write of one whole string.  Several threads reporting at once
        // otherwise interleave in the middle of a message.
        std::cerr << o.str() << std::flush;
    }

    /** A descriptor that closes itself unless somebody takes it. */
    class held_fd {
    public:
        explicit held_fd(int fd) : m_fd(fd) {}

        ~held_fd() { if(m_fd >= 0) ::close(m_fd); }

        held_fd(const held_fd&) = delete;
        held_fd& operator=(const held_fd&) = delete;

        /** Hand it over; this no longer closes it. */
        int release() { const int fd = m_fd; m_fd = -1; return fd; }

    private:
        int m_fd;
    };

}

server::server(deferred_handler_t, listener l, tls_context tls,
               const policy& p)
    : m_listener(std::move(l)),
      m_on_error(complain),
      m_tls(std::move(tls)),
      m_policy(p),
      m_jobs(static_cast<int>(p.threads))
{
    // So the accept never blocks: the reactor reports the listening descriptor
    // ready, and a client that sends an RST between the readiness and the
    // accept would otherwise leave it waiting for the next one.
    m_listener.set_blocking(false);

    // Registered once, here, rather than a two-element pollfd array rebuilt on
    // every serve_one.  The reactor's own wake pipe is the second descriptor
    // now, and it registers that itself.
    m_listen = m_reactor.add(
        m_listener.get_socket(), reactor::READ,
        [this](reactor::token, int, reactor::event_type) {
            // Caught rather than thrown: a reactor callback that throws goes to
            // on_error and the pass carries on, where serve_one's caller has
            // always been told about a failed accept.  Parked, and rethrown
            // once the pass is over.
            try {
                m_accepted = m_async ? accept_and_start() : accept_and_post();
            }
            catch(...) { m_accept_error = std::current_exception(); }
        });
}

server::server(listener l, handler h, tls_context tls, const policy& p)
    : server(deferred_handler_t(), std::move(l), std::move(tls), p)
{
    m_handler = std::move(h);

    // At construction, not at first serve.  A server built wrong should say so
    // when it is built, and routing this through the tagged constructor above
    // is what lets the async one delegate without disarming it.
    if(!m_handler) throw exception("a server with no handler");
}

server::server(unsigned short port, handler h, const std::string& host,
               tls_context tls, const policy& p)
    : server(listener(port, host), std::move(h), std::move(tls), p)
{}

server::server(listener l, async_handler h, tls_context tls, const policy& p)
    : server(deferred_handler_t(), std::move(l), std::move(tls), p)
{
    m_async = std::move(h);

    if(!m_async) throw exception("a server with no handler");
}

server::address_count::hold::~hold() {
    if(m_owner) m_owner->release(m_who);
}

server::address_count::hold&
server::address_count::hold::operator=(hold&& o) {
    if(this != &o) {
        if(m_owner) m_owner->release(m_who);

        m_owner = o.m_owner;
        m_who = std::move(o.m_who);
        o.m_owner = 0;
    }

    return *this;
}

server::address_count::hold
server::address_count::take(const std::string& who, std::size_t cap) {
    std::lock_guard<std::mutex> got(m_lock);

    if(cap != 0) {
        const std::map<std::string, std::size_t>::const_iterator i =
            m_counts.find(who);

        if(i != m_counts.end() && i->second >= cap) return hold();
    }

    m_counts[who]++;

    return hold(*this, who);
}

void server::address_count::release(const std::string& who) {
    std::lock_guard<std::mutex> got(m_lock);

    const std::map<std::string, std::size_t>::iterator i = m_counts.find(who);

    if(i == m_counts.end()) return;

    // Erased at zero rather than left holding one, which is what keeps the
    // table the size of the live connections instead of the size of everyone
    // who has ever connected.
    if(--i->second == 0) m_counts.erase(i);
}

std::size_t server::address_count::count(const std::string& who) const {
    std::lock_guard<std::mutex> got(m_lock);

    const std::map<std::string, std::size_t>::const_iterator i =
        m_counts.find(who);

    return i == m_counts.end() ? 0 : i->second;
}

std::size_t server::address_count::tracked() const {
    std::lock_guard<std::mutex> got(m_lock);

    return m_counts.size();
}

std::size_t server::connections_from(const std::string& address) const {
    return m_addresses.count(address);
}

bool server::admit(int fd, const peer& from, address_count::hold& into) {
    into = m_addresses.take(from.address, m_policy.max_per_address);

    if(into.engaged()) return true;

    // Accepted and closed, because there is no way to know who is calling
    // without accepting the call -- see policy::max_per_address.  Silent: this
    // layer has no protocol to refuse in.
    ::close(fd);

    return false;
}

void server::report(const std::exception& e, const peer& from) const {
    m_on_error(e, from);
}

bool server::full() const {
    return m_live.size() >= (m_policy.max_connections != 0
                             ? m_policy.max_connections : 1);
}

server::server(unsigned short port, async_handler h, const std::string& host,
               tls_context tls, const policy& p)
    : server(listener(port, host), std::move(h), std::move(tls), p)
{}

server::~server() {
    stop();
    join();
}

unsigned short server::port() const { return m_listener.port(); }

bool server::tls() const { return !m_tls.empty(); }

std::size_t server::pending() const { return m_jobs.size(); }

void server::on_error(error_handler h) {
    m_on_error = h ? h : error_handler(complain);
}

bool server::stopped() const { return m_stop.load(); }

std::size_t server::cap() const {
    // With no pool the cap is one -- the connection being handled -- so the
    // admission predicate below reads the same in both modes and serve_one
    // needs no branch.  Nothing is ever queued there, so the wait answers
    // pred(0), which is 0 < 1.
    if(m_policy.threads == 0) return 1;

    return m_policy.max_queued != 0 ? m_policy.max_queued : m_policy.threads;
}

void server::stop(bool drain) {
    m_stop.store(true);

    // What drain decides is the fate of jobs still queued -- connections
    // accepted but not yet started.  Abandoning one destroys the
    // shared_ptr<held_fd> it carries, so the descriptor closes rather than
    // leaks (see serve_one, which names this as one of the three drop paths
    // that indirection buys).  A handler already running is not affected.
    //
    // Kept out of the enumeration below because it is a separate question: the
    // call wakes a parked thread whichever way drain goes.

    // Three ways a thread can be blocked in here, and shutdown hangs if any one
    // is missed.  This is the second: a thread parked in the queue's depth
    // wait, whose predicate ORs in the queue's own exit flag.
    m_jobs.stop(drain);

    // And the connections themselves, for an async server: each is a
    // coroutine parked on a read that may never complete, and before #212 a
    // token could not end one.  Requesting it resumes the coroutine, which
    // throws cancelled out of the await and unwinds -- closing the descriptor
    // on the way, because held_fd lives in the frame.
    //
    // **On the reactor's thread, and not on this one.**  That unwind is not
    // bookkeeping: it runs the connection's own code, destroys its frame,
    // closes its descriptor and takes it out of m_live -- every one of which
    // the reactor thread is also doing, to the same connection.  Doing it from
    // whichever thread happened to call stop() is a data race, and this is
    // what it cost: ThreadSanitizer clean on a server whose connections each
    // answered one request and finished long before anybody stopped it, and
    // not clean at all once keep-alive left them parked and waiting.
    //
    // The reactor's own stop() goes *inside* the posted job rather than after
    // it.  Posting and then stopping would be a race the other way: run()
    // re-reads the flag at the top of every pass, so the pass that would have
    // run this job may never happen, and the connections would never be ended.
    if(m_reactor.on_reactor_thread()) {
        // Either this *is* the reactor's thread -- stop() from a handler -- or
        // nobody has claimed it and there is no other thread to race with.
        cancel_live();

        m_reactor.stop();
    }
    else {
        m_reactor.post([this]{
            cancel_live();

            // And the third way a thread can be blocked in here: one waiting
            // in the reactor.  It owns the wake pipe now, and the
            // byte-down-a-pipe this used to write by hand went with it.
            m_reactor.stop();
        });
    }
}

void server::cancel_live() {
    for(std::list<live>::iterator i = m_live.begin(); i != m_live.end(); ++i) {
        try { i->token.request(); }
        catch(std::exception&) { /* already requested */ }
    }

    reap();
}

void server::join() {
    // No stop() here, deliberately -- see the header.  The workers wait for
    // work forever, so a caller who has not stopped will hang; a stop() added
    // here to prevent that would have to pick a drain argument, and picking one
    // overwrites what a caller who did stop already asked for.
    m_jobs.join();
}

void server::serve(int fd, const peer& from, address_count::hold slot) {
    // `slot` is taken **by value and never touched again**, which is the whole
    // of the bookkeeping: it is destroyed when this returns, on every path it
    // can return by.  A local copy of it was here until a break harness showed
    // that removing the local broke nothing -- because the parameter was
    // already doing the work.
    held_fd held(fd);

    // A pooled thread keeps OpenSSL's per-thread error queue between
    // connections, where a thread per connection got a fresh one.  open_ssl()
    // clears before the handshake, so that path is safe; this is for a
    // mid-connection SSL_read or SSL_write failure, which drains the queue to
    // build its message and would otherwise report residue left by whatever
    // this worker did last.
    ERR_clear_error();

    try {
        std::unique_ptr<socketstream> s;

        // Constructed here, on the worker, not before the dispatch: building a
        // tlsstream performs SSL_accept, and doing that on the accept thread
        // would let one slow or hostile client stall every other connection
        // through a full handshake -- which is exactly what a pool is for.
        if(!m_tls.empty()) {
            s.reset(new tlsstream(tls_server, m_tls, adopt, held.release(),
                                  from.address, from.port, m_policy.io_timeout));
        }
        else {
            s.reset(new socketstream(adopt, held.release(), from.address,
                                     from.port, m_policy.io_timeout));
        }

        try {
            m_handler(*s, from);
        }
        catch(std::exception& e) {
            m_on_error(e, from);
        }
        catch(...) {
            const exception unknown("a handler threw something that is not an "
                                    "exception");

            m_on_error(unknown, from);
        }

        s->flush();
        s->close();
    }
    catch(std::exception& e) {
        // The outer one: constructing the stream, which for a TLS server is the
        // handshake.  A connection that fails here must not take the loop with
        // it.
        m_on_error(e, from);
    }
    catch(...) {
        const exception unknown("a connection failed with something that is "
                                "not an exception");

        m_on_error(unknown, from);
    }
}

bool server::accept_and_post() {
    peer from;
    int fd;

    try {
        fd = m_listener.accept(from, 0);
    }
    catch(std::exception& e) {
        // Out of descriptors is not a reason to stop serving, and it is a
        // condition that clears.  Without the pause this spins at full tilt,
        // because the connection stays queued and the listener stays readable.
        //
        // The reactor makes the better answer easy -- modify(m_listen, NONE)
        // and an after() to re-arm -- but that only works when the accept loop
        // is reactor-driven rather than call-driven, and serve_one(timeout) is
        // call-driven by contract.  Left as it was; named as the follow-up.
        if(errno == EMFILE || errno == ENFILE) {
            m_on_error(e, from);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            return false;
        }

        throw;
    }

    if(fd < 0) return false;

    address_count::hold slot;

    // Refused before anything else touches the descriptor -- no TLS handshake,
    // no job posted, no stream built.  A flood should cost an accept and a
    // close and nothing more.
    if(!admit(fd, from, slot)) return true;

    // A shared_ptr, and it is not shared: refcount one for its whole life,
    // except while job_queue copies the std::function around.  The indirection
    // is doing two jobs.  post() takes a std::function, which requires a
    // *copyable* callable, so a move-only holder captured by value will not
    // compile -- and std::move_only_function is C++23 and absent from both
    // toolchains here.  And a held_fd captured by value into a non-mutable
    // lambda would be const, so release() would not compile either.
    //
    // What it buys: every path that drops the job closes the descriptor by
    // RAII.  post() returning early because the queue stopped, a worker
    // abandoning queued jobs on stop(false), post() itself throwing -- a bare
    // int leaks in all three, and a stop() landing between the accept and the
    // post is exactly that race.
    std::shared_ptr<held_fd> held = std::make_shared<held_fd>(fd);

    // The same indirection, and for the first of the same two reasons: post()
    // takes a std::function, which must be copyable, and a hold is move-only.
    std::shared_ptr<address_count::hold> kept =
        std::make_shared<address_count::hold>(std::move(slot));

    m_jobs.post([this, held, from, kept] {
        serve(held->release(), from, std::move(*kept));
    });

    return true;
}


bool server::accept_and_start() {
    peer from;
    int fd;

    try {
        fd = m_listener.accept(from, 0);
    }
    catch(std::exception& e) {
        if(errno == EMFILE || errno == ENFILE) {
            m_on_error(e, from);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            return false;
        }

        throw;
    }

    if(fd < 0) return false;

    address_count::hold slot;

    if(!admit(fd, from, slot)) return true;

    // Its own, so stop() can end this connection without ending every other,
    // and so a handler can hang a deadline on it.
    const cancel_token t = cancel_token::create();

    // The coroutine is created before it is owned and moved into the list
    // afterwards.  Lazy, so nothing runs until start() below -- which matters,
    // because a coroutine that ran on creation would run before anything held
    // its frame.
    m_live.push_back(live{ serve_async(fd, from, t, std::move(slot)), t });

    m_live.back().work.start();

    if(full()) {
        // Stop asking about the listener rather than accepting and refusing.
        m_reactor.modify(m_listen, reactor::NONE);

        m_listen_off = true;
    }

    return true;
}

void server::reap() {
    for(std::list<live>::iterator i = m_live.begin(); i != m_live.end(); ) {
        if(i->work.done()) i = m_live.erase(i);
        else ++i;
    }

    if(m_listen_off && !full()) {
        m_reactor.modify(m_listen, reactor::READ);

        m_listen_off = false;
    }
}

namespace {

    /**
     * Request `t` if this outlives `seconds`.
     *
     * The same shape as net::http::server's armed_deadline, including the one
     * subtle part: **cancel only from the reactor's own thread.** A
     * destructor runs wherever the frame is destroyed, and one path that
     * destroys this frame is stop() requesting the token from another thread,
     * so the coroutine unwinds there while the reactor is mid-pass.
     * reactor::cancel() mutates two containers with no lock, and cancelling
     * from that unwind is a data race whose symptom is heap corruption a long
     * way from the cause.
     *
     * Abandoning the timer instead is safe and cheap: it fires later into a
     * token that has already been requested, and requesting a requested token
     * does nothing.
     */
    class timed_out_after {
    public:
        timed_out_after(reactor& r, double seconds, cancel_token t)
            : m_reactor(&r)
        {
            if(seconds > 0) {
                m_timer = deadline(
                    r,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::duration<double>(seconds)),
                    t);
            }
        }

        ~timed_out_after() {
            if(m_timer == reactor::timer_token::none) return;

            if(m_reactor->on_reactor_thread()) m_reactor->cancel(m_timer);

            m_timer = reactor::timer_token::none;
        }

        timed_out_after(const timed_out_after&) = delete;
        timed_out_after& operator=(const timed_out_after&) = delete;

    private:
        reactor*             m_reactor;
        reactor::timer_token m_timer = reactor::timer_token::none;
    };

}

task<void> server::serve_async(int fd, peer from, cancel_token t,
                               address_count::hold slot) {
    // By value again, and here that means *in the coroutine frame*: a
    // coroutine's parameters are moved into the frame and destroyed with it,
    // so the count is released when the connection's frame is -- which reap()
    // does, on every path the connection can end on.

    // Owns the descriptor for the whole coroutine, including the paths where
    // it is destroyed while suspended -- a stop(), or the task being dropped
    // -- because destroying the frame destroys this.
    held_fd held(fd);

    // The reactor's thread serves every connection, so OpenSSL's per-thread
    // error queue is shared between all of them rather than between the
    // connections one pool thread happened to take.  Cleared for the same
    // reason the blocking path clears it, more so.
    ERR_clear_error();

    try {
        if(!m_tls.empty()) {
            // The handshake is awaited rather than performed in a
            // constructor, which is the whole reason async_tls exists: a slow
            // or hostile client stalls itself and nothing else.
            async_tls tls(m_reactor, fd, tls_server, m_tls, t);

            // **And it is bounded, which it was not** (#240).
            //
            // "Stalls itself and nothing else" was true about the reactor
            // thread and false about the connection slot. Nothing timed the
            // handshake: net::http::server's three timeouts are all armed
            // *after* it returns, so a client that connected and said nothing
            // held a slot indefinitely -- measured at six seconds and still
            // held, against an initial_idle_timeout of one. Each such
            // connection spends one of max_connections and one of
            // max_per_address, which is a slow-loris wearing the one costume
            // the timeouts could not see.
            //
            // The blocking path never had this: it hands io_timeout to
            // tlsstream, where it becomes SO_RCVTIMEO and covers SSL_accept.
            // Same number here, for the same reason, so the two agree about
            // what a handshake may cost.
            {
                const timed_out_after bound(m_reactor, m_policy.io_timeout, t);

                co_await tls.handshake();
            }

            connection c(m_reactor, m_jobs, tls.reader(), tls.writer(), from, t);

            co_await m_async(c, from);

            // Bounded for the same reason: a peer that will not finish the
            // close is a peer holding a slot, and the handler's own deadlines
            // are gone by now.
            {
                const timed_out_after bound(m_reactor, m_policy.io_timeout, t);

                co_await tls.shutdown();
            }
        }
        else {
            async_fd_reader r(m_reactor, fd, t);
            async_fd_writer w(m_reactor, fd, t);

            connection c(m_reactor, m_jobs, r, w, from, t);

            co_await m_async(c, from);
        }
    }
    catch(cancelled&) {
        // Not a failure.  A connection ends this way when the server stops or
        // when a handler's own deadline fires, and both are decisions rather
        // than errors -- reporting them would put a line on stderr for every
        // connection on every shutdown.
    }
    catch(std::exception& e) {
        m_on_error(e, from);
    }
    catch(...) {
        const exception unknown("a handler threw something that is not an "
                                "exception");

        m_on_error(unknown, from);
    }
}

bool server::serve_one(double timeout) {
    if(m_stop.load()) return false;

    // Connections that have finished let go of their descriptors here, once a
    // pass, rather than at some point inside a coroutine that has no good
    // place to remove itself from a list it is running out of.  Re-arms the
    // listener too, if a slot has come free.
    if(m_async) reap();

    // Admission control before the wait, so an overflow connection waits in the
    // kernel's listen backlog rather than here, accepted and holding a
    // descriptor.  The queue holds the depth and the lock; what counts as full
    // is this caller's opinion, and this is the one line that has one.
    //
    // An async server has no queue and nothing to wait *for*: a slot frees
    // when a coroutine finishes, which needs this same thread to keep turning
    // the reactor.  Its cap is enforced by disabling the listener's
    // registration instead; see accept_and_start.
    const std::size_t room = cap();
    const auto full = [room](std::size_t depth) { return depth < room; };

    // One budget across both waits, not one each.  The header says the timeout
    // covers admission control as well as the accept, and without a deadline
    // serve_one(10) could spend ten seconds waiting for room and ten more in
    // the reactor -- twenty, for a caller who asked for ten.
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(timeout));

    if(!m_async) {
        const bool have_room = timeout > 0
            ? m_jobs.wait(full,
                          std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::duration<double>(timeout)))
            : m_jobs.wait(full);

        if(!have_room) return false;
    }

    // The two conventions meet here and nowhere else: serve_one spells
    // "forever" as 0, as listener::accept and basic_socketbuf's timeouts do,
    // and the reactor spells it negative so that 0 can mean "drain what is
    // ready and return".  reactor.hh says why.
    std::chrono::nanoseconds wait = reactor::forever;

    if(timeout > 0) {
        const auto left = deadline - std::chrono::steady_clock::now();

        if(left <= std::chrono::steady_clock::duration::zero()) return false;

        wait = std::chrono::duration_cast<std::chrono::nanoseconds>(left);
    }

    m_accepted = false;

    m_reactor.run_one(wait);

    // Rethrown here rather than from the callback, so that an accept failure
    // still reaches serve_one's caller as it always has.  Inside the reactor
    // it would have gone to on_error and the pass would have carried on.
    if(m_accept_error) {
        const std::exception_ptr e = m_accept_error;

        m_accept_error = nullptr;

        std::rethrow_exception(e);
    }

    return m_accepted;
}

void server::run() {
    // No m_stop.store(false) here.  job_queue::stop() retires the pool and
    // nothing respawns it, so a "restarted" server would accept connections and
    // drop every one in silence -- post() returns early once the queue has
    // stopped.  One-shot, and said so in the header.
    while(!m_stop.load()) {
        serve_one(0);
    }
}

}
}
