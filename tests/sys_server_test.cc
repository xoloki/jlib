/* -*- mode: C++ c-basic-offset: 4 -*-
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
 */

// sys::server -- accept a connection, maybe secure it, hand it to a handler.
//
// The assertions worth having are about the awkward parts, not the echo: that a
// handler which throws does not take the loop with it, that the thread cap is a
// cap and not a suggestion, that stop() returns promptly rather than after a
// poll interval, and that the destructor does not return while a handler is
// still using what it captured.  Those are the ways a server goes wrong quietly.
//
// Entirely in one process, including the TLS section: the certificate is
// generated at runtime and the client's trust store points at it, so this runs
// on a developer's machine and not only in the build container.

#include "certificate.hh"

#include <jlib/sys/listener.hh>
#include <jlib/sys/server.hh>
#include <jlib/sys/sync.hh>
#include <jlib/sys/socketstream.hh>

#include <csignal>
#include <memory>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <jlib/sys/sslstream.hh>
#include <jlib/sys/tls.hh>

#include <atomic>
#include <condition_variable>
#include <map>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace sys = jlib::sys;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

/** Send one line and read one back.  "" if anything went wrong. */
static std::string ask(unsigned short port, const std::string& what) {
    try {
        sys::socketstream s("127.0.0.1", port, 5);

        s.set_timeout(5);
        s << what << "\r\n" << std::flush;

        std::string line;
        std::getline(s, line);

        while(!line.empty() && line.back() == '\r') line.pop_back();

        return line;
    }
    catch(std::exception&) {
        return std::string();
    }
}

static void echo(sys::socketstream& s, const sys::peer&) {
    std::string line;
    std::getline(s, line);

    while(!line.empty() && line.back() == '\r') line.pop_back();

    s << "echo: " << line << "\r\n" << std::flush;
}

static void one_connection_at_a_time() {
    std::cout << "\none connection at a time:\n";

    sys::server srv(0, echo);

    ok("it bound a port", srv.port() != 0, std::to_string(srv.port()));
    ok("and it is not a TLS server", !srv.tls());

    // Nothing is connecting, so this must come back rather than wait.
    const auto start = std::chrono::steady_clock::now();

    ok("serve_one with nothing waiting returns false", !srv.serve_one(0.3));
    ok("and takes about as long as it was told to",
       seconds_since(start) >= 0.2 && seconds_since(start) < 3.0,
       std::to_string(seconds_since(start)) + "s");

    std::string got;
    std::thread client([&srv, &got] { got = ask(srv.port(), "hello"); });

    ok("and true when one is", srv.serve_one(5));

    client.join();

    ok("the handler ran and the answer came back", got == "echo: hello", got);

    // The peer really is filled in, on the path a server actually uses.
    sys::peer seen;
    sys::server watcher(0, [&seen](sys::socketstream& s, const sys::peer& from) {
        seen = from;
        s << "ok\r\n" << std::flush;
    });

    std::thread second([&watcher] { ask(watcher.port(), "x"); });
    watcher.serve_one(5);
    second.join();

    ok("the handler is told who connected",
       seen.address == "127.0.0.1" && seen.port != 0 && seen.loopback(),
       seen.address + ":" + std::to_string(seen.port));
}

static void a_handler_that_throws_does_not_stop_the_server() {
    std::cout << "\na handler that throws does not stop the server:\n";

    std::atomic<int> served{0};
    std::atomic<int> reported{0};

    sys::server srv(0, [&served](sys::socketstream& s, const sys::peer&) {
        std::string line;
        std::getline(s, line);

        served++;

        if(line.find("boom") != std::string::npos)
            throw std::runtime_error("the handler gave up");

        s << "fine\r\n" << std::flush;
    });

    srv.on_error([&reported](const std::exception&, const sys::peer&) {
        reported++;
    });

    std::thread first([&srv] { ask(srv.port(), "boom"); });
    srv.serve_one(5);
    first.join();

    ok("the throw is reported", reported.load() == 1,
       std::to_string(reported.load()));

    // The one that matters: the loop is still alive.
    std::string got;
    std::thread second([&srv, &got] { got = ask(srv.port(), "again"); });

    ok("and the server serves the next connection", srv.serve_one(5));

    second.join();

    ok("which gets its own answer", got == "fine", got);
    ok("both connections reached the handler", served.load() == 2,
       std::to_string(served.load()));
}

static void several_at_once_when_asked() {
    std::cout << "\nseveral at once, when asked:\n";

    std::atomic<int> live{0};
    std::atomic<int> most{0};
    std::atomic<int> done{0};

    sys::server::policy p;
    p.threads = 4;

    sys::server srv(0, [&live, &most, &done](sys::socketstream& s, const sys::peer&) {
        const int now = ++live;

        int seen = most.load();
        while(now > seen && !most.compare_exchange_weak(seen, now))
            ;

        std::string line;
        std::getline(s, line);

        // Long enough that serial handling would show up as one at a time.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        s << "slow\r\n" << std::flush;

        live--;
        done++;
    }, "127.0.0.1", sys::tls_context(), p);

    const int clients = 12;
    std::vector<std::thread> them;

    for(int i = 0; i < clients; i++)
        them.emplace_back([&srv] { ask(srv.port(), "go"); });

    const auto start = std::chrono::steady_clock::now();

    for(int i = 0; i < clients; i++) srv.serve_one(10);

    // stop() then join(), in that order.  join() waits for the pool to retire
    // and does not tell it to leave, so joining a pooled server without
    // stopping it first waits for something that never happens.
    srv.stop();
    srv.join();

    const double took = seconds_since(start);

    for(std::thread& t : them) t.join();

    ok("every client is served", done.load() == clients,
       std::to_string(done.load()) + "/" + std::to_string(clients));

    // The direct assertion that this is concurrent at all.  Serially, twelve
    // handlers of 150ms take at least 1.8 seconds and the maximum overlap is 1.
    ok("more than one handler ran at once", most.load() > 1,
       "peak " + std::to_string(most.load()));

    ok("and it took less than serving them one by one would",
       took < clients * 0.15, std::to_string(took) + "s");

    // The cap is a cap.  Four threads means never five.
    ok("and never more than the cap", most.load() <= 4,
       "peak " + std::to_string(most.load()));
}

static void the_cap_holds_at_two() {
    std::cout << "\nthe cap holds:\n";

    std::atomic<int> live{0};
    std::atomic<int> most{0};
    std::atomic<int> done{0};

    // Two, so a breach is unambiguous rather than a scheduling artefact.
    sys::server::policy p;
    p.threads = 2;

    sys::server srv(0, [&live, &most, &done](sys::socketstream& s, const sys::peer&) {
        const int now = ++live;

        int seen = most.load();
        while(now > seen && !most.compare_exchange_weak(seen, now))
            ;

        std::string line;
        std::getline(s, line);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        s << "done\r\n" << std::flush;

        live--;
        done++;
    }, "127.0.0.1", sys::tls_context(), p);

    const int clients = 10;
    std::vector<std::thread> them;

    for(int i = 0; i < clients; i++)
        them.emplace_back([&srv] { ask(srv.port(), "go"); });

    for(int i = 0; i < clients; i++) srv.serve_one(10);

    srv.stop();
    srv.join();

    for(std::thread& t : them) t.join();

    ok("never three handlers at once", most.load() <= 2,
       "peak " + std::to_string(most.load()));

    // Nothing is dropped: the ones that did not fit waited in the kernel's
    // listen backlog, which is where a connection is supposed to wait.
    ok("and none of the ten was dropped", done.load() == clients,
       std::to_string(done.load()) + "/" + std::to_string(clients));
}

static void the_two_paths_became_one() {
    std::cout << "\nserial and threaded are one path now:\n";

    // The acceptance test for the whole rewrite.  The old server had an
    // if(threads == 0) fork in serve_one -- one branch calling the handler
    // inline, one dispatching it.  job_queue(0) runs a job on the thread that
    // posts it, so there is one path and a number, and this is what proves the
    // number still means what it did.
    std::thread::id where;

    {
        // threads defaults to 0.
        sys::server srv(0, [&where](sys::socketstream& s, const sys::peer&) {
            where = std::this_thread::get_id();

            std::string line;
            std::getline(s, line);

            s << "inline\r\n" << std::flush;
        });

        std::thread client([&srv] { ask(srv.port(), "go"); });

        srv.serve_one(5);
        client.join();

        ok("with no pool the handler runs on the accept thread",
           where == std::this_thread::get_id());
    }

    std::thread::id elsewhere;

    {
        sys::server::policy p;
        p.threads = 1;

        sys::server srv(0, [&elsewhere](sys::socketstream& s, const sys::peer&) {
            elsewhere = std::this_thread::get_id();

            std::string line;
            std::getline(s, line);

            s << "pooled\r\n" << std::flush;
        }, "127.0.0.1", sys::tls_context(), p);

        std::thread client([&srv] { ask(srv.port(), "go"); });

        srv.serve_one(5);
        client.join();
        srv.stop();
        srv.join();

        ok("and with a pool of one it does not",
           elsewhere != std::this_thread::get_id());
    }
}

static void a_busy_pool_is_not_a_full_queue() {
    std::cout << "\na busy pool is not a full queue:\n";

    // The cap counts what is *waiting*, not what is running.  If a job that had
    // been taken still counted, one slow handler on a pool of one would make
    // the queue look full and stall the accept loop against work already under
    // way -- which is the whole reason size() is the queue's depth and nothing
    // else.
    sys::sync<bool> go(false);

    sys::server::policy p;
    p.threads = 1;
    p.max_queued = 1;

    sys::server srv(0, [&go](sys::socketstream& s, const sys::peer&) {
        std::string line;
        std::getline(s, line);

        std::unique_lock<std::mutex> lock(go.mutex());

        go.wait(lock, [&go] { return go(); });

        s << "held\r\n" << std::flush;
    }, "127.0.0.1", sys::tls_context(), p);

    std::thread first([&srv] { ask(srv.port(), "one"); });

    ok("the first connection is accepted", srv.serve_one(5));

    // The worker has it, so nothing is queued -- and the loop must be willing
    // to accept again even though the pool is entirely busy.
    for(int i = 0; i < 200 && srv.pending() != 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    ok("and with it running, nothing is queued", srv.pending() == 0,
       std::to_string(srv.pending()));

    std::thread second([&srv] { ask(srv.port(), "two"); });

    const auto start = std::chrono::steady_clock::now();

    ok("so a second is accepted with the pool busy", srv.serve_one(5));
    ok("promptly, rather than waiting for the first to finish",
       seconds_since(start) < 2.0, std::to_string(seconds_since(start)) + "s");

    ok("and now one is queued", srv.pending() == 1,
       std::to_string(srv.pending()));

    // A third would have to wait: depth 1 is the cap.  stop() has to release
    // that wait, which is what the exit term in job_queue's own predicate is
    // for -- without it the accept thread sleeps through the shutdown.
    std::atomic<bool> returned{false};
    std::atomic<bool> accepted{true};

    std::thread third([&srv, &returned, &accepted] {
        accepted = srv.serve_one(0);
        returned = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ok("a third accept waits for room", !returned.load());

    srv.stop();

    for(int i = 0; i < 200 && !returned.load(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    ok("and stop() releases it", returned.load());
    ok("which reports that it accepted nothing", !accepted.load());

    go.set(true);

    third.join();
    first.join();
    second.join();
}

/**
 * The counter, on its own.
 *
 * Per-address separation is the whole point of the cap and it cannot be
 * reached through a server: every client here connects over loopback, and both
 * platforms report the peer as 127.0.0.1 whichever loopback address is dialled
 * -- measured on macOS, where 127.0.0.2 does not route, and in the Linux
 * container, where it routes but the kernel still picks 127.0.0.1 as the
 * source.  So the bookkeeping is tested here with addresses supplied as
 * strings, and the section after it tests the wiring over real connections.
 */
static void counting_connections_per_address() {
    std::cout << "\ncounting connections per address:\n";

    {
        sys::server::address_count c;

        sys::server::address_count::hold a = c.take("10.0.0.1", 2);
        sys::server::address_count::hold b = c.take("10.0.0.1", 2);

        ok("two fit under a cap of two", a.engaged() && b.engaged());

        {
            sys::server::address_count::hold third = c.take("10.0.0.1", 2);

            ok("and a third does not", !third.engaged());
        }

        // The whole point: one address at its limit is not everybody's limit.
        sys::server::address_count::hold other = c.take("10.0.0.2", 2);

        ok("while another address is unaffected", other.engaged());

        ok("and they are counted apart",
           c.count("10.0.0.1") == 2 && c.count("10.0.0.2") == 1,
           std::to_string(c.count("10.0.0.1")) + " and " +
           std::to_string(c.count("10.0.0.2")));
    }

    {
        sys::server::address_count c;

        {
            sys::server::address_count::hold a = c.take("10.0.0.1", 1);

            ok("one is at the cap", c.count("10.0.0.1") == 1 &&
               !c.take("10.0.0.1", 1).engaged());
        }

        // A count that drifts upward refuses that address forever, which is
        // the failure this RAII exists to make impossible.
        ok("and releasing it lets the next one in",
           c.take("10.0.0.1", 1).engaged());
    }

    {
        sys::server::address_count c;

        {
            sys::server::address_count::hold a = c.take("10.0.0.7", 0);
            sys::server::address_count::hold b = c.take("10.0.0.7", 0);
            sys::server::address_count::hold d = c.take("10.0.0.7", 0);

            ok("a cap of zero is no cap at all",
               a.engaged() && b.engaged() && d.engaged());
        }

        // Bounded by construction: an entry exists only while a connection
        // does.  There is nothing here to sweep, which is the difference from
        // the rate limiter's table.
        ok("and the table empties itself as connections end",
           c.tracked() == 0, std::to_string(c.tracked()));
    }

    {
        sys::server::address_count c;

        sys::server::address_count::hold a = c.take("10.0.0.9", 4);
        sys::server::address_count::hold moved(std::move(a));

        ok("moving a hold moves the count, it does not copy it",
           c.count("10.0.0.9") == 1 && moved.engaged() && !a.engaged(),
           std::to_string(c.count("10.0.0.9")));
    }
}

/** Connect and read the greeting the handler sends.  "" if refused. */
static std::string greeted(unsigned short port,
                           std::unique_ptr<sys::socketstream>& into)
{
    try {
        into.reset(new sys::socketstream("127.0.0.1", port, 5));

        into->set_timeout(5);

        std::string line;

        std::getline(*into, line);

        while(!line.empty() && line.back() == '\r') line.pop_back();

        return line;
    }
    catch(std::exception&) {
        return std::string();
    }
}

/** Wait, briefly and one-sidedly, for the server to agree. */
static bool settles_to(sys::server& srv, const char* who, std::size_t want) {
    for(int i = 0; i < 200; i++) {
        if(srv.connections_from(who) == want) return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return false;
}

static void one_address_may_not_have_every_slot(bool async) {
    std::cout << "\none address may not have every slot ("
              << (async ? "async" : "blocking") << "):\n";

    sys::server::policy p;

    p.threads = 4;
    p.max_connections = 16;
    p.max_per_address = 2;

    // Greets, then waits: the connection stays up until the client goes away,
    // which is what lets the cap be observed rather than raced.
    std::unique_ptr<sys::server> srv;

    if(async) {
        srv.reset(new sys::server(
            0,
            [](sys::server::connection& c, const sys::peer&) -> sys::task<void> {
                co_await c.writer().write("hello\r\n");

                (void) co_await c.reader().fill();
            },
            "127.0.0.1", sys::tls_context(), p));
    }
    else {
        srv.reset(new sys::server(
            0,
            [](sys::socketstream& s, const sys::peer&) {
                s << "hello\r\n" << std::flush;

                std::string line;

                std::getline(s, line);
            },
            "127.0.0.1", sys::tls_context(), p));
    }

    std::thread th([&srv] { srv->run(); });

    std::unique_ptr<sys::socketstream> a, b, third, fourth;

    ok("  the first connection is served", greeted(srv->port(), a) == "hello");
    ok("  and so is the second", greeted(srv->port(), b) == "hello");

    // Accepted and closed, silently: there is no protocol at this layer to
    // refuse in, so what the client sees is end of stream.
    ok("  the third is refused, with nothing said",
       greeted(srv->port(), third).empty());

    ok("  and the server counts two, not three",
       settles_to(*srv, "127.0.0.1", 2),
       std::to_string(srv->connections_from("127.0.0.1")));

    // Closing lets the handler return, which releases the hold.  A count that
    // did not come back down would refuse this address forever.
    a.reset();

    ok("  closing one brings the count back down",
       settles_to(*srv, "127.0.0.1", 1),
       std::to_string(srv->connections_from("127.0.0.1")));

    ok("  and the slot it freed is usable",
       greeted(srv->port(), fourth) == "hello");

    b.reset();
    fourth.reset();
    third.reset();

    ok("  and when they all go, nothing is left counted",
       settles_to(*srv, "127.0.0.1", 0),
       std::to_string(srv->connections_from("127.0.0.1")));

    srv->stop();
    srv->join();

    th.join();
}

static void stopping() {
    std::cout << "\nstopping:\n";

    sys::server srv(0, echo);

    std::thread loop([&srv] { srv.run(); });

    // Let it get into the poll.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto start = std::chrono::steady_clock::now();

    srv.stop();
    loop.join();

    const double took = seconds_since(start);

    // Promptly, which is the whole reason for the wake pipe: a loop that
    // polled with a short timeout instead would take up to that timeout, and a
    // loop that closed its own listening descriptor would be racing.
    ok("run() returns as soon as stop() is called", took < 1.0,
       std::to_string(took) + "s");

    ok("and the server says it is stopped", srv.stopped());

    // From inside a handler, which a joining stop() could not allow.
    sys::server inner(0, [&inner](sys::socketstream& s, const sys::peer&) {
        std::string line;
        std::getline(s, line);

        s << "bye\r\n" << std::flush;

        inner.stop();
    });

    std::thread inner_loop([&inner] { inner.run(); });
    std::thread client([&inner] { ask(inner.port(), "quit"); });

    inner_loop.join();
    client.join();

    ok("and stop() works from inside a handler", inner.stopped());
}

static void stopping_without_draining() {
    std::cout << "\nstopping without draining:\n";

    std::atomic<int> ran{0};

    // One worker and a queue with room, so all three connections are posted at
    // once and the two behind the first are still waiting when the stop lands.
    sys::server::policy p;
    p.threads = 1;
    p.max_queued = 4;

    const int clients = 3;

    std::vector<std::string> answers(clients);
    std::vector<std::thread> them;

    double took = 0;

    {
        sys::server srv(0, [&ran](sys::socketstream& s, const sys::peer&) {
            ran++;

            std::string line;
            std::getline(s, line);

            std::this_thread::sleep_for(std::chrono::milliseconds(400));

            s << "served\r\n" << std::flush;
        }, "127.0.0.1", sys::tls_context(), p);

        for(int i = 0; i < clients; i++)
            them.emplace_back([&srv, &answers, i] {
                answers[i] = ask(srv.port(), "go");
            });

        for(int i = 0; i < clients; i++) srv.serve_one(10);

        const auto start = std::chrono::steady_clock::now();

        srv.stop(false);
        srv.join();

        took = seconds_since(start);
    }

    for(std::thread& t : them) t.join();

    // The one already running still finishes: drain is about queued work, not
    // about cancelling a handler mid-flight.  So this waits for that one and
    // not for the two behind it.
    ok("join() waits for the running handler and not for the queue",
       took < clients * 0.4, std::to_string(took) + "s");

    ok("the queued connections never reached the handler", ran.load() < clients,
       std::to_string(ran.load()) + "/" + std::to_string(clients) + " ran");

    int answered = 0;

    for(const std::string& a : answers) if(!a.empty()) answered++;

    // Closed rather than left hanging.  The descriptor rides in the job's
    // shared_ptr<held_fd>, so dropping the job destroys it and the client gets
    // an immediate EOF; ask() would otherwise sit on its own five-second
    // timeout, and this section would take five seconds rather than one.
    ok("and their clients were closed rather than left waiting",
       answered < clients,
       std::to_string(answered) + " of " + std::to_string(clients) + " answered");
}

static void the_destructor_waits_for_a_handler() {
    std::cout << "\nthe destructor waits for a handler:\n";

    // A detached handler reaches the server through the handler it was given,
    // so a destructor that returned while one was running would be pulling the
    // ground out from under it.
    std::atomic<bool> finished{false};
    std::atomic<bool> destroyed_first{false};

    {
        sys::server::policy p;
        p.threads = 2;

        sys::server srv(0, [&finished](sys::socketstream& s, const sys::peer&) {
            std::string line;
            std::getline(s, line);

            std::this_thread::sleep_for(std::chrono::milliseconds(250));

            s << "slow\r\n" << std::flush;

            finished = true;
        }, "127.0.0.1", sys::tls_context(), p);

        std::thread client([&srv] { ask(srv.port(), "go"); });

        srv.serve_one(5);

        // The handler is running on its own thread now.  Leaving this scope
        // must not return until it has finished.
        srv.stop();

        if(!finished.load()) destroyed_first = true;

        client.join();
    }

    ok("the handler was still running when the scope ended",
       destroyed_first.load(), "otherwise this proves nothing");

    ok("and the destructor did not return until it had finished",
       finished.load());
}

/**
 * A client that connects to a TLS server and never speaks.
 *
 * **This was unbounded on the async server** and it is the #240 find that the
 * header was actively wrong about: three timeouts were advertised as bounding
 * a slow-loris, and every one of them is armed by the *handler*, after the
 * handshake has returned. Nothing timed the handshake itself, so a peer that
 * completed a TCP connection and then said nothing held its slot for as long
 * as it cared to -- measured at six seconds and still held, against an
 * initial_idle_timeout of one.
 *
 * The blocking server never had it: `io_timeout` reaches `tlsstream` as
 * `SO_RCVTIMEO`, which covers `SSL_accept`. Both are asserted here, because
 * "the other one was always fine" is the kind of claim worth a test rather
 * than a sentence.
 *
 * Each such connection spends one of `max_connections` and one of
 * `max_per_address`, so the cheapest possible client -- a connect and nothing
 * else, no TLS, no bytes -- could exhaust an async server.
 */
static void a_handshake_that_never_finishes(bool async) {
    std::cout << "\na TLS client that says nothing ("
              << (async ? "async" : "blocking") << "):\n";

    const std::string cert = "handshake_test_cert.pem";
    const std::string key = "handshake_test_key.pem";

    if(!make_cert(cert, key)) {
        std::cout << "  skip  could not generate a test certificate\n";

        return;
    }

    sys::server::policy p;

    // Short, so "bounded" and "unbounded" are far apart rather than a race.
    p.io_timeout = 2;
    p.threads = 2;

    std::unique_ptr<sys::server> srv;

    if(async) {
        srv.reset(new sys::server(
            0,
            [](sys::server::connection&, const sys::peer&) -> sys::task<void> {
                co_return;
            },
            "127.0.0.1", sys::tls_context::server(cert, key), p));
    }
    else {
        srv.reset(new sys::server(0, echo, "127.0.0.1",
                                  sys::tls_context::server(cert, key), p));
    }

    srv->on_error([](const std::exception&, const sys::peer&) {});

    std::thread th([&srv] { srv->run(); });

    // A bare TCP connection.  No ClientHello, no bytes at all: the cheapest
    // thing a client can do that still occupies a server.
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in to;

    std::memset(&to, 0, sizeof to);

    to.sin_family = AF_INET;
    to.sin_port = htons(srv->port());

    ::inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);

    const bool connected =
        ::connect(fd, reinterpret_cast<struct sockaddr*>(&to), sizeof to) == 0;

    ok("  the connection is accepted", connected);

    // Generous: six times the timeout.  The assertion is one-sided -- that it
    // is dropped *at all* -- so a slow machine cannot make it fail, only an
    // unbounded handshake can.
    bool gone = false;

    for(int i = 0; i < 120 && !gone; i++) {
        char b[1];

        if(::recv(fd, b, 1, MSG_DONTWAIT) == 0) gone = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ok("  and a handshake that never starts is eventually dropped", gone);

    ok("  which returns the slot it was holding",
       settles_to(*srv, "127.0.0.1", 0),
       std::to_string(srv->connections_from("127.0.0.1")));

    if(fd >= 0) ::close(fd);

    srv->stop();
    srv->join();
    th.join();

    ::unlink(cert.c_str());
    ::unlink(key.c_str());
}

/**
 * The server context refuses renegotiation.
 *
 * A policy assertion rather than a behavioural one, and worth being clear
 * about which: this checks the option is set, not that a renegotiating client
 * is refused. Driving a real renegotiation needs a client built to do
 * something no client in this tree does, and the option *is* the mechanism --
 * OpenSSL enforces it, and what could go wrong is somebody deleting the line.
 * A test that fails when the line goes is the thing worth having.
 *
 * Why it is set: TLS 1.3 has no renegotiation, TLS 1.2 does, and this context
 * allows 1.2. Client-initiated renegotiation is cheap to ask for and expensive
 * to answer, at a rate the client picks -- a server doing an attacker's
 * arithmetic. Its one real use, asking for a client certificate mid-connection,
 * is out of scope here.
 */
static void the_server_context_refuses_renegotiation() {
    std::cout << "\nthe TLS server context's policy:\n";

    const std::string cert = "renego_test_cert.pem";
    const std::string key = "renego_test_key.pem";

    if(!make_cert(cert, key)) {
        std::cout << "  skip  could not generate a test certificate\n";

        return;
    }

    const sys::tls_context ctx = sys::tls_context::server(cert, key);

    SSL* ssl = ctx.new_ssl();

    ok("  renegotiation is off", ssl != 0 &&
       (SSL_get_options(ssl) & SSL_OP_NO_RENEGOTIATION) != 0);

    // And the floor it was already holding, asserted beside it so the two
    // policies live in one place.
    ok("  and TLS 1.2 is the floor", ssl != 0 &&
       SSL_get_min_proto_version(ssl) == TLS1_2_VERSION,
       ssl ? std::to_string(SSL_get_min_proto_version(ssl)) : "");

    if(ssl) SSL_free(ssl);

    ::unlink(cert.c_str());
    ::unlink(key.c_str());
}

/**
 * Which certificate a client gets, and the name it asked for.
 *
 * **The client here is raw OpenSSL, deliberately.** jlib's own `tlsstream`
 * sends SNI taken from the host it is connecting to, so it cannot ask for
 * `a.example` while dialling 127.0.0.1 -- and what is under test is the
 * server's choice, not the client's convenience.
 *
 * The certificate is chosen during the handshake because that is the only
 * moment available: it goes out before the request that carries `Host`
 * arrives. A server with two names on one port that cannot do this presents
 * the wrong certificate and a careful client stops before saying anything.
 */
static std::string cert_offered_for(unsigned short port, const char* sni) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());

    if(!ctx) return "no client context";

    // Not verifying: this asks *which* certificate arrived, and every one of
    // them is self-signed by the test.
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, 0);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in to;

    std::memset(&to, 0, sizeof to);

    to.sin_family = AF_INET;
    to.sin_port = htons(port);

    ::inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);

    std::string got = "no connection";

    if(::connect(fd, reinterpret_cast<struct sockaddr*>(&to), sizeof to) == 0) {
        SSL* ssl = SSL_new(ctx);

        SSL_set_fd(ssl, fd);

        if(sni) SSL_set_tlsext_host_name(ssl, sni);

        if(SSL_connect(ssl) == 1) {
            X509* peer = SSL_get1_peer_certificate(ssl);

            if(peer) {
                char name[256];

                X509_NAME_oneline(X509_get_subject_name(peer), name,
                                  sizeof name);

                got = name;

                X509_free(peer);
            }
            else got = "no certificate";
        }
        else got = "handshake failed";

        SSL_free(ssl);
    }

    ::close(fd);
    SSL_CTX_free(ctx);

    return got;
}

static void a_certificate_per_name() {
    std::cout << "\na certificate per name:\n";

    const std::string dcert = "sni_default_cert.pem";
    const std::string dkey = "sni_default_key.pem";
    const std::string acert = "sni_a_cert.pem";
    const std::string akey = "sni_a_key.pem";

    if(!make_cert(dcert, dkey, "default.example", "DNS:default.example") ||
       !make_cert(acert, akey, "a.example", "DNS:a.example"))
    {
        std::cout << "  skip  could not generate test certificates\n";

        return;
    }

    sys::tls_context tls = sys::tls_context::server(dcert, dkey);

    tls.add_site("a.example", acert, akey);

    sys::server srv(0, echo, "127.0.0.1", tls);

    srv.on_error([](const std::exception&, const sys::peer&) {});

    std::thread th([&srv] { srv.run(); });

    struct { const char* sni; const char* want; const char* why; } cases[] = {
        { "a.example", "a.example", "a name with a certificate of its own" },
        { "A.EXAMPLE", "a.example", "and the same name shouted, since DNS is "
                                    "not case-sensitive" },
        { "other.example", "default.example",
          "a name nothing claimed keeps the default, rather than failing" },
        { 0, "default.example", "and so does a client that sends no SNI" }
    };

    for(std::size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const std::string got = cert_offered_for(srv.port(), cases[i].sni);

        ok(std::string("  ") + cases[i].why,
           got.find(cases[i].want) != std::string::npos, got);
    }

    srv.stop();
    srv.join();
    th.join();

    ::unlink(dcert.c_str());
    ::unlink(dkey.c_str());
    ::unlink(acert.c_str());
    ::unlink(akey.c_str());
}

static void over_tls() {
    std::cout << "\nover TLS:\n";

    const std::string cert = "server_test_cert.pem";
    const std::string key = "server_test_key.pem";

    if(!make_cert(cert, key)) {
        std::cout << "  skip  could not generate a test certificate\n";

        return;
    }

    const char* const had = std::getenv("SSL_CERT_FILE");
    const std::string keep = had ? had : "";

    ::setenv("SSL_CERT_FILE", cert.c_str(), 1);

    try {
        sys::server srv(0, echo, "127.0.0.1", sys::tls_context::server(cert, key));

        ok("it says it is a TLS server", srv.tls());

        std::string got;

        std::thread client([&srv, &got] {
            try {
                // "localhost" is the name the certificate covers.
                sys::tlsstream s("localhost", srv.port());

                s.set_timeout(5);
                s << "secure\r\n" << std::flush;

                std::getline(s, got);

                while(!got.empty() && got.back() == '\r') got.pop_back();
            }
            catch(std::exception&) {}
        });

        srv.serve_one(10);
        client.join();

        ok("a TLS client is served through the same handler", got == "echo: secure",
           got);

        // A handshake that fails must not take the loop with it -- the same
        // assertion as the throwing handler, one layer down.
        std::atomic<int> reported{0};

        srv.on_error([&reported](const std::exception&, const sys::peer&) {
            reported++;
        });

        std::thread plain([&srv] {
            try {
                sys::socketstream s("127.0.0.1", srv.port(), 5);

                s << "GET / HTTP/1.1\r\n\r\n" << std::flush;
                s.close();
            }
            catch(std::exception&) {}
        });

        srv.serve_one(10);
        plain.join();

        ok("a plaintext client is reported, not fatal", reported.load() == 1,
           std::to_string(reported.load()));

        std::string after;

        std::thread again([&srv, &after] {
            try {
                sys::tlsstream s("localhost", srv.port());

                s.set_timeout(5);
                s << "still here\r\n" << std::flush;

                std::getline(s, after);

                while(!after.empty() && after.back() == '\r') after.pop_back();
            }
            catch(std::exception&) {}
        });

        srv.serve_one(10);
        again.join();

        ok("and the next TLS connection is served", after == "echo: still here",
           after);
    }
    catch(std::exception& e) {
        ok("a TLS server runs", false, e.what());
    }

    if(keep.empty()) ::unsetenv("SSL_CERT_FILE");
    else             ::setenv("SSL_CERT_FILE", keep.c_str(), 1);

    std::remove(cert.c_str());
    std::remove(key.c_str());
}



/**
 * Runs a server on its own thread and **always joins it**.
 *
 * Not a convenience. A bare `std::thread` destroyed while still joinable calls
 * std::terminate, so a section that threw between starting the thread and
 * joining it -- a failed connect, a handshake that did not happen -- aborted
 * the process instead of reporting the failure. That is worth a type rather
 * than care, because the abort *looks* like a crash in the code under test:
 * it cost a round of blaming the library for taking the accept thread down
 * when what had died was the test's own thread handle.
 */
struct turning {
    explicit turning(sys::server& s) : m_s(s), m_t([&s] { s.run(); }) {}

    ~turning() { done(); }

    /** Stop and join, once; the destructor calls it on the throwing path. */
    void done() {
        if(!m_t.joinable()) return;

        m_s.stop();
        m_t.join();
    }

    turning(const turning&) = delete;
    turning& operator=(const turning&) = delete;

private:
    sys::server& m_s;
    std::thread  m_t;
};

/**
 * Two ports, one server.
 *
 * The thing under test is not "it binds twice" -- that part is arithmetic.
 * It is that **TLS belongs to the listener**, so one server can hold a
 * plaintext port and an encrypted one and answer both through the same
 * handler, with one reactor, one pool and one cap between them.  Two servers
 * would give two of each, which is what made 80-and-443 two processes.
 */
static void two_ports_one_server() {
    std::cout << "\ntwo ports, one server:\n";

    const std::string cert = "server_test_two_cert.pem";
    const std::string key = "server_test_two_key.pem";

    if(!make_cert(cert, key)) {
        std::cout << "  skip  could not generate a test certificate\n";

        return;
    }

    const char* const had = std::getenv("SSL_CERT_FILE");
    const std::string keep = had ? had : "";

    ::setenv("SSL_CERT_FILE", cert.c_str(), 1);

    try {
        // What the handler saw, so the assertions below are about the peer
        // rather than about which socket happened to answer.
        std::mutex said;
        std::map<unsigned short, bool> secure_on;
        std::map<unsigned short, unsigned short> local_on;

        const sys::server::handler note =
            [&said, &secure_on, &local_on](sys::socketstream& s,
                                           const sys::peer& from) {
                {
                    std::lock_guard<std::mutex> hold(said);

                    secure_on[from.local_port] = from.secure;
                    local_on[from.local_port] = from.local_port;
                }

                echo(s, from);
            };

        std::vector<sys::server::bound> ports;

        ports.push_back(sys::server::bound(sys::listener(0, "127.0.0.1")));
        ports.push_back(sys::server::bound(sys::listener(0, "127.0.0.1"),
                                           sys::tls_context::server(cert, key)));

        const unsigned short plain = ports[0].l.port();
        const unsigned short tls = ports[1].l.port();

        sys::server srv(std::move(ports), note);

        ok("it bound two", srv.listeners() == 2, std::to_string(srv.listeners()));
        ok("and reports both ports",
           srv.ports().size() == 2 && srv.ports()[0] == plain &&
               srv.ports()[1] == tls,
           std::to_string(srv.ports()[0]) + " and " + std::to_string(srv.ports()[1]));

        // **port() and tls() describe the same listener.**  Read as "does this
        // server speak TLS anywhere", tls() would be true here and port()
        // would still be the plaintext port -- and a caller doing
        // `if(s.tls()) connect_tls(s.port())` would be wrong while each half
        // was right.
        ok("port() is the first of them", srv.port() == plain,
           std::to_string(srv.port()));
        ok("and tls() answers about that same one, not about any of them",
           !srv.tls() && srv.tls(1));
        ok("which the indexed pair agrees with",
           srv.port(0) == plain && srv.port(1) == tls && !srv.tls(0));

        turning turn(srv);

        ok("the plaintext port is served", ask(plain, "one") == "echo: one");

        {
            sys::tlsstream s("localhost", tls);

            s.set_timeout(5);
            s << "two\r\n" << std::flush;

            std::string line;
            std::getline(s, line);

            while(!line.empty() && line.back() == '\r') line.pop_back();

            ok("and so is the TLS one, through the same handler",
               line == "echo: two", line);
        }

        {
            std::lock_guard<std::mutex> hold(said);

            ok("the handler was told which port each arrived on",
               local_on.size() == 2 && local_on.count(plain) &&
                   local_on.count(tls));

            // The bit an http-to-https redirect is made of.  Without it a
            // handler serving both ports cannot tell them apart at all.
            ok("and that one was encrypted and the other was not",
               secure_on[plain] == false && secure_on[tls] == true);
        }

        turn.done();

        ok("and stop() ends a two-port server just the same", srv.stopped());
    }
    catch(std::exception& e) {
        ok("two ports, one server", false, e.what());
    }

    if(had) ::setenv("SSL_CERT_FILE", keep.c_str(), 1);
    else    ::unsetenv("SSL_CERT_FILE");

    std::remove(cert.c_str());
    std::remove(key.c_str());
}

/**
 * A full server stops accepting on **every** port.
 *
 * The cap counts connections, not connections-per-port.  A version that
 * disarmed only the listener that happened to fill it would pass every
 * single-port test in this file and hand a client the whole cap a second time
 * for the price of knocking on the other door -- which is worth a test of its
 * own precisely because nothing else here would notice.
 */
static void a_full_server_stops_accepting_everywhere() {
    std::cout << "\nwhen it is full, every port is:\n";

    sys::server::policy p;

    p.threads = 2;
    p.max_connections = 2;

    std::atomic<int> arrived{0};

    // **Held by suspending, not by blocking.**  An async handler runs on the
    // reactor thread, so a condition variable here would stop the accept loop
    // itself and the second connection would never arrive -- which is what the
    // first draft of this test did, and it looked exactly like a cap that was
    // working.  Awaiting a read parks the coroutine and leaves the reactor
    // turning, so the connection is held open by a client that says nothing.
    const sys::server::async_handler hold =
        [&arrived](sys::server::connection& c, const sys::peer&)
            -> sys::task<void> {
        arrived++;

        co_await c.reader().fill();

        co_await c.writer().write("held\r\n");
    };

    std::vector<sys::server::bound> ports;

    ports.push_back(sys::server::bound(sys::listener(0, "127.0.0.1")));
    ports.push_back(sys::server::bound(sys::listener(0, "127.0.0.1")));

    const unsigned short first = ports[0].l.port();
    const unsigned short second = ports[1].l.port();

    sys::server srv(std::move(ports), hold, p);

    turning turn(srv);

    // Fill the cap on the first port only.
    std::vector<std::unique_ptr<sys::socketstream> > held;

    for(int i = 0; i < 2; i++) {
        held.push_back(std::unique_ptr<sys::socketstream>(
            new sys::socketstream("127.0.0.1", first, 5)));
    }

    for(int i = 0; i < 100 && arrived.load() < 2; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    ok("two connections fill it", arrived.load() == 2,
       std::to_string(arrived.load()));

    // **The other port must now be deaf.**  The connect itself may well
    // succeed -- the kernel completes the handshake into the listen backlog
    // whether or not anything accepts -- so what is asserted is that no third
    // connection reaches the handler.
    {
        sys::socketstream other("127.0.0.1", second, 5);

        other << "hello\r\n" << std::flush;

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        ok("and the second port accepts nothing while it is full",
           arrived.load() == 2, std::to_string(arrived.load()) + " arrived");
    }

    // Dropping the clients ends the reads the handlers are parked on, which
    // ends the connections and frees the slots.
    held.clear();

    // Once the cap clears, both are armed again -- including the one that was
    // never the reason it filled.
    bool back = false;

    for(int i = 0; i < 200 && !back; i++) {
        if(arrived.load() > 2) { back = true; break; }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ok("and the queued one is taken once there is room again", back,
       std::to_string(arrived.load()) + " arrived");

    turn.done();
}

int main() {
    // **This test is an application, and applications ignore SIGPIPE.**
    //
    // It plays TLS client with raw OpenSSL -- deliberately, because jlib's own
    // client takes its SNI from the host it dials and cannot ask for one name
    // while connecting to another. A raw socket gets neither SO_NOSIGPIPE nor
    // any of the library's sigpipe_guards, so OpenSSL writing to a server that
    // has closed kills this process on Linux. Traced to exactly that with a
    // backtrace: libssl -> BIO_write -> write, from this binary.
    //
    // Not a mask over a library defect: every SSL call in jlib that can write
    // is guarded, and this branch fixed three that were not. It is the same
    // line jhttpd and jserve both carry, for the same reason.
    std::signal(SIGPIPE, SIG_IGN);

    std::cout << std::unitbuf;

    one_connection_at_a_time();
    a_handler_that_throws_does_not_stop_the_server();
    several_at_once_when_asked();
    two_ports_one_server();
    a_full_server_stops_accepting_everywhere();
    the_cap_holds_at_two();
    counting_connections_per_address();
    one_address_may_not_have_every_slot(false);
    one_address_may_not_have_every_slot(true);
    the_two_paths_became_one();
    a_busy_pool_is_not_a_full_queue();
    stopping();
    stopping_without_draining();
    the_destructor_waits_for_a_handler();
    the_server_context_refuses_renegotiation();
    a_handshake_that_never_finishes(false);
    a_handshake_that_never_finishes(true);
    a_certificate_per_name();
    over_tls();

    // What a green run does not establish.
    //
    // That this is safe on a public port.  Nothing here is hardened against a
    // slow-loris, a flood, or a client that connects and never speaks, beyond
    // a thread cap, the listen backlog and a read timeout.  It is a server for
    // a loopback redirect and a test harness and should stay one.
    //
    // Not the timing assertions under load.  "More than one handler ran at
    // once" and "it took less than serving them one by one" are wall-clock
    // claims on a machine that may be busy; they are written with wide margins,
    // and a failure means look at the machine before looking at the code.
    //
    // Not client certificates, which the context deliberately neither asks for
    // nor examines.
    //
    // Not that an abandoned connection's descriptor is closed at the OS level.
    // "Stopping without draining" infers it from the client seeing an
    // immediate EOF, which is what a close looks like from the far end but is
    // also what it would look like if the process had exited.  Only a
    // descriptor count across the whole section would say it properly.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
