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
#include <jlib/sys/sslstream.hh>
#include <jlib/sys/tls.hh>

#include <atomic>
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

int main() {
    std::cout << std::unitbuf;

    one_connection_at_a_time();
    a_handler_that_throws_does_not_stop_the_server();
    several_at_once_when_asked();
    the_cap_holds_at_two();
    the_two_paths_became_one();
    a_busy_pool_is_not_a_full_queue();
    stopping();
    the_destructor_waits_for_a_handler();
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
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
