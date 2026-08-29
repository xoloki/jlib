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

#include <jlib/sys/sslstream.hh>

#include <openssl/err.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

#include <poll.h>
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

server::server(listener l, handler h, tls_context tls, const policy& p)
    : m_listener(std::move(l)),
      m_handler(std::move(h)),
      m_on_error(complain),
      m_tls(std::move(tls)),
      m_policy(p),
      m_jobs(static_cast<int>(p.threads))
{
    if(!m_handler) throw exception("a server with no handler");

    // So the accept never blocks: serve_one polls the listening descriptor and
    // the wake pipe together, and a client that sends an RST between the poll
    // and the accept would otherwise leave it waiting for the next one.
    m_listener.set_blocking(false);
}

server::server(unsigned short port, handler h, const std::string& host,
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

void server::stop() {
    m_stop.store(true);

    // Three ways a thread can be blocked in here, and shutdown hangs if any one
    // is missed.  This is the second: a thread parked in the queue's depth
    // wait, whose predicate ORs in the queue's own exit flag.
    //
    // Drained, so a connection already accepted is still served: it is not the
    // client's fault that we are going away.
    m_jobs.stop(true);

    // And the third: a thread in poll(2).  A byte down the pipe rather than
    // closing the listening descriptor, which is undefined while another thread
    // polls it and on macOS does not wake it.
    try { m_wake.write_int(1); }
    catch(std::exception&) { /* full is fine; one byte is all it takes */ }
}

void server::join() {
    // The stop is what makes this terminate.  job_queue's workers wait for work
    // indefinitely, so joining them without telling them to leave waits for
    // something that never happens.  Draining, so nothing accepted is
    // discarded; idempotent, so ~server calling stop() first costs nothing.
    m_jobs.stop(true);
    m_jobs.join();
}

void server::serve(int fd, const peer& from) {
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

bool server::serve_one(double timeout) {
    if(m_stop.load()) return false;

    // Admission control before the poll, so an overflow connection waits in the
    // kernel's listen backlog rather than here, accepted and holding a
    // descriptor.  The queue holds the depth and the lock; what counts as full
    // is this caller's opinion, and this is the one line that has one.
    const std::size_t room = cap();
    const auto full = [room](std::size_t depth) { return depth < room; };

    // One budget across both waits, not one each.  The header says the timeout
    // covers admission control as well as the accept, and without a deadline
    // serve_one(10) could spend ten seconds waiting for room and ten more in
    // poll(2) -- twenty, for a caller who asked for ten.
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(timeout));

    const bool have_room = timeout > 0
        ? m_jobs.wait(full, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::duration<double>(timeout)))
        : m_jobs.wait(full);

    if(!have_room) return false;

    struct pollfd fds[2];

    fds[0].fd = m_listener.get_socket();
    fds[0].events = POLLIN;
    fds[0].revents = 0;

    fds[1].fd = m_wake.get_reader();
    fds[1].events = POLLIN;
    fds[1].revents = 0;

    int ms = -1;

    if(timeout > 0) {
        const auto left = deadline - std::chrono::steady_clock::now();

        if(left <= std::chrono::steady_clock::duration::zero()) return false;

        ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(left).count());
    }

    int r;

    while((r = ::poll(fds, 2, ms)) < 0 && errno == EINTR)
        ;

    if(r < 0)
        throw exception(std::string("error in poll(): ") + std::strerror(errno));

    if(r == 0) return false;

    if(fds[1].revents & POLLIN) {
        // Drain it; the pipe's read end is non-blocking.
        try {
            for(;;) m_wake.read_int();
        }
        catch(std::exception&) {}

        return false;
    }

    if(!(fds[0].revents & POLLIN)) return false;

    peer from;
    int fd;

    try {
        fd = m_listener.accept(from, 0);
    }
    catch(std::exception& e) {
        // Out of descriptors is not a reason to stop serving, and it is a
        // condition that clears.  Without the pause this spins at full tilt,
        // because the connection stays queued and the poll stays readable.
        if(errno == EMFILE || errno == ENFILE) {
            m_on_error(e, from);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            return false;
        }

        throw;
    }

    if(fd < 0) return false;

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

    m_jobs.post([this, held, from] { serve(held->release(), from); });

    return true;
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
