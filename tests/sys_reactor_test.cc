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
/*
 * sys_reactor_test -- the multiplexer jlib did not have.
 *
 * Determinism comes from two rules, and everything here follows from them.
 *
 * **Cause the readiness in this thread, before the wait.**  Write to a pipe
 * and then run_one(): the descriptor is already readable before the backend is
 * entered, so there is no race, no sleep, and no dependence on the scheduler.
 *
 * **Assert on what was dispatched, not on how long it took.**  Where a
 * duration must be checked at all it is checked one-sidedly -- a timer was not
 * *early* -- which cannot flake on a loaded machine.
 *
 * Every section runs against every backend the build has, selected through
 * JLIB_SYS_REACTOR before the reactor is constructed, with the name in each
 * detail string.  A fallback that compiles and is never run is a fallback that
 * does not work.
 */

#include "feed.hh"

#include <jlib/sys/reactor.hh>
#include <jlib/sys/pipe.hh>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace sys = jlib::sys;

static int failures = 0;
static std::string backend;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what << " [" << backend << "]";
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static const std::chrono::milliseconds SHORT{200};
static const std::chrono::seconds LONG{5};

/** Nothing registered and nothing due. */
static void nothing_to_do() {
    sys::reactor r;

    ok("an empty pass does nothing", !r.run_one(SHORT));

    ok("and one with no budget returns at once",
       !r.run_one(std::chrono::nanoseconds::zero()));

    // No registrations, no timers, no jobs, no work guard: there is no event
    // that could ever arrive, so this must return rather than hang.
    r.run();

    ok("run() on an empty reactor returns rather than hanging", true);

    ok("it starts with nothing registered", r.count() == 0,
       std::to_string(r.count()));
}

/**
 * Level-triggered, which is a contract and not a default.
 *
 * The one section that catches EPOLLET or EV_CLEAR, and it needs no clock.
 * A callback that reads half of what is waiting must be called again, because
 * basic_socketbuf::underflow reads once into a fixed buffer and cannot
 * accumulate across two calls.
 */
static void level_not_edge() {
    sys::reactor r;
    sys::pipe p(false, false);

    int fired = 0;

    r.add(p.get_reader(), sys::reactor::READ,
          [&fired](sys::reactor::token, int, sys::reactor::event_type) {
              fired++;
          });

    p.write_int(1);

    r.run_one(LONG);

    ok("a readable descriptor dispatches", fired == 1, std::to_string(fired));

    // Deliberately not consumed.  Under edge triggering this pass sees
    // nothing and the assertion below fails; under level triggering the
    // descriptor is still readable and says so again.
    r.run_one(LONG);

    ok("and keeps dispatching while it stays readable", fired == 2,
       std::to_string(fired));

    p.read_int();

    ok("but not once it has been drained", !r.run_one(SHORT) && fired == 2,
       std::to_string(fired));
}

/**
 * A callback that removes itself, and then touches what it captured.
 *
 * The entry is held by shared_ptr across the call for exactly this: erasing
 * the map entry must not free the std::function being executed.  Without a
 * sanitizer a naive implementation may pass this silently, which is said again
 * in the closing note.
 */
static void a_callback_removes_itself() {
    sys::reactor r;
    sys::pipe p(false, false);

    int fired = 0;
    bool survived = false;

    r.add(p.get_reader(), sys::reactor::READ,
          [&r, &fired, &survived](sys::reactor::token t, int,
                                  sys::reactor::event_type) {
              fired++;

              r.remove(t);

              // Reading a capture after the entry has been erased.
              survived = (fired == 1);
          });

    p.write_int(1);

    r.run_one(LONG);

    ok("it ran", fired == 1, std::to_string(fired));

    ok("and could still read its own captures afterwards", survived);

    ok("the registration is gone", r.count() == 0, std::to_string(r.count()));

    // Still readable, and must not be dispatched again.
    ok("and it is not called again", !r.run_one(SHORT) && fired == 1,
       std::to_string(fired));
}

/**
 * A callback that removes a *different* descriptor, in the same pass.
 *
 * Both are readable before the wait, so both are in one ready list.  Whichever
 * runs first must suppress the other -- that is remove()'s guarantee holding
 * within a pass, and it is what makes it safe to destroy what a callback
 * captured.
 *
 * Asserted as "whichever went first, the other did not", because the reactor
 * does not promise an order within a pass and a test that assumed one would be
 * a flake waiting on a backend change.
 */
static void a_callback_removes_another() {
    sys::reactor r;
    sys::pipe a(false, false);
    sys::pipe b(false, false);

    int ran_a = 0;
    int ran_b = 0;

    sys::reactor::token ta = sys::reactor::token::none;
    sys::reactor::token tb = sys::reactor::token::none;

    ta = r.add(a.get_reader(), sys::reactor::READ,
               [&](sys::reactor::token, int, sys::reactor::event_type) {
                   ran_a++;
                   r.remove(tb);
               });

    tb = r.add(b.get_reader(), sys::reactor::READ,
               [&](sys::reactor::token, int, sys::reactor::event_type) {
                   ran_b++;
                   r.remove(ta);
               });

    a.write_int(1);
    b.write_int(1);

    r.run_one(LONG);

    ok("exactly one of the two ran", ran_a + ran_b == 1,
       "a=" + std::to_string(ran_a) + " b=" + std::to_string(ran_b));

    ok("and the other was removed before it could", r.count() == 1,
       std::to_string(r.count()));
}

/** A registration added during a pass waits for the next one. */
static void an_addition_waits_for_the_next_pass() {
    sys::reactor r;
    sys::pipe first(false, false);
    sys::pipe second(false, false);

    int late = 0;

    r.add(first.get_reader(), sys::reactor::READ,
          [&](sys::reactor::token t, int, sys::reactor::event_type) {
              r.remove(t);

              r.add(second.get_reader(), sys::reactor::READ,
                    [&late](sys::reactor::token, int, sys::reactor::event_type) {
                        late++;
                    });
          });

    // Both readable before the wait, so the second would be in this pass's
    // ready list if the list were consulted rather than decided.
    first.write_int(1);
    second.write_int(1);

    r.run_one(LONG);

    ok("the new registration does not run in the pass that made it",
       late == 0, std::to_string(late));

    r.run_one(LONG);

    ok("and does on the next", late == 1, std::to_string(late));
}

/**
 * A descriptor number reused after a close reaches the right registration.
 *
 * The only section that catches a backend keying its table on the descriptor
 * instead of the token, and it is deterministic by the lowest-free-descriptor
 * rule.
 */
static void a_reused_descriptor_is_not_the_old_one() {
    sys::reactor r;

    int gone = 0;
    int fresh = 0;
    int old_fd = -1;

    {
        sys::pipe first(false, false);

        old_fd = first.get_reader();

        const sys::reactor::token t =
            r.add(old_fd, sys::reactor::READ,
                  [&gone](sys::reactor::token, int, sys::reactor::event_type) {
                      gone++;
                  });

        r.remove(t);
    }

    sys::pipe second(false, false);

    if(second.get_reader() != old_fd) {
        std::cout << "  skip  the descriptor was not reused [" << backend
                  << "]\n";
        return;
    }

    r.add(second.get_reader(), sys::reactor::READ,
          [&fresh](sys::reactor::token, int, sys::reactor::event_type) {
              fresh++;
          });

    second.write_int(1);

    r.run_one(LONG);

    ok("the new registration ran", fresh == 1, std::to_string(fresh));

    ok("and the closed one's callback did not", gone == 0,
       std::to_string(gone));
}

/** post() and wake(), from another thread. */
static void posting_from_another_thread() {
    sys::reactor r;

    std::atomic<bool> ran(false);
    std::thread::id on;

    const std::thread::id here = std::this_thread::get_id();

    std::thread t([&r, &ran, &on]{
        r.post([&ran, &on]{
            on = std::this_thread::get_id();
            ran = true;
        });
    });

    // Blocked here with nothing registered; only the posted job can end it.
    r.run_one(LONG);

    t.join();

    ok("a posted job runs", ran.load());

    ok("on the reactor's thread, not the poster's", on == here);
}

/** Nothing posted is lost, however the wake byte is coalesced. */
static void nothing_posted_is_lost() {
    sys::reactor r;
    sys::reactor::work keep(r);

    std::atomic<int> done(0);

    const int THREADS = 4;
    const int EACH = 1000;

    std::vector<std::thread> posters;

    for(int i = 0; i < THREADS; i++) {
        posters.push_back(std::thread([&r, &done]{
            for(int j = 0; j < EACH; j++) r.post([&done]{ done++; });
        }));
    }

    while(done.load() < THREADS * EACH) r.run_one(LONG);

    for(std::size_t i = 0; i < posters.size(); i++) posters[i].join();

    // Drain anything that landed between the last check and the joins.
    while(r.run_one(std::chrono::nanoseconds::zero())) ;

    ok("every job ran exactly once", done.load() == THREADS * EACH,
       std::to_string(done.load()) + " of " +
       std::to_string(THREADS * EACH));
}

/**
 * A posted remove() suppresses the same pass's callback.
 *
 * Pins the documented ordering: jobs, then ready descriptors, then timers.
 */
static void a_posted_remove_beats_the_dispatch() {
    sys::reactor r;
    sys::pipe p(false, false);

    int fired = 0;

    const sys::reactor::token t =
        r.add(p.get_reader(), sys::reactor::READ,
              [&fired](sys::reactor::token, int, sys::reactor::event_type) {
                  fired++;
              });

    p.write_int(1);

    r.post([&r, t]{ r.remove(t); });

    r.run_one(LONG);

    ok("the job ran before the descriptor was dispatched", fired == 0,
       std::to_string(fired));
}

static void stopping() {
    {
        sys::reactor r;
        sys::pipe never(false, false);

        r.add(never.get_reader(), sys::reactor::READ,
              [](sys::reactor::token, int, sys::reactor::event_type) {});

        std::thread t([&r]{
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            r.stop();
        });

        const auto start = std::chrono::steady_clock::now();

        r.run();

        const double took =
            std::chrono::duration<double>(std::chrono::steady_clock::now()
                                          - start).count();

        t.join();

        ok("stop() from another thread ends run()", took < 5.0,
           std::to_string(took) + "s");

        ok("and it says so afterwards", r.stopped());

        // Sticky: a loop that could be restarted invites a restart that
        // silently drops everything registered before the stop.
        r.run();

        ok("run() after a stop returns at once", true);
    }

    {
        sys::reactor r;
        sys::pipe p(false, false);

        r.add(p.get_reader(), sys::reactor::READ,
              [&r](sys::reactor::token, int, sys::reactor::event_type) {
                  r.stop();
              });

        p.write_int(1);

        r.run();

        ok("stop() from inside a callback ends run()", r.stopped());
    }
}

static void timers() {
    {
        sys::reactor r;

        std::vector<int> order;

        r.after(std::chrono::milliseconds(90),
                [&order](sys::reactor::timer_token) { order.push_back(90); });
        r.after(std::chrono::milliseconds(30),
                [&order](sys::reactor::timer_token) { order.push_back(30); });
        r.after(std::chrono::milliseconds(60),
                [&order](sys::reactor::timer_token) { order.push_back(60); });

        ok("three are armed", r.timers() == 3, std::to_string(r.timers()));

        while(order.size() < 3) r.run_one(LONG);

        ok("they fire by deadline and not by arming order",
           order.size() == 3 && order[0] == 30 && order[1] == 60 &&
           order[2] == 90,
           std::to_string(order.size()) + " fired");

        ok("and are gone afterwards", r.timers() == 0,
           std::to_string(r.timers()));
    }

    {
        sys::reactor r;

        bool fired = false;
        const auto armed = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point when;

        r.after(std::chrono::milliseconds(50),
                [&](sys::reactor::timer_token) {
                    fired = true;
                    when = std::chrono::steady_clock::now();
                });

        while(!fired) r.run_one(LONG);

        // One-sided.  A loaded machine can make this late and must not make it
        // fail; being *early* is the only thing that would be wrong.
        ok("a timer is never early", when - armed >= std::chrono::milliseconds(50),
           std::to_string(std::chrono::duration<double>(when - armed).count())
           + "s");
    }

    {
        sys::reactor r;

        int fired = 0;

        const sys::reactor::timer_token t =
            r.after(std::chrono::milliseconds(20),
                    [&fired](sys::reactor::timer_token) { fired++; });

        r.cancel(t);

        r.run_one(std::chrono::milliseconds(120));

        ok("a cancelled timer does not fire", fired == 0,
           std::to_string(fired));

        ok("and cancelling it twice is not an error",
           (r.cancel(t), true));
    }

    {
        sys::reactor r;

        int ticks = 0;

        r.every(std::chrono::milliseconds(10),
                [&ticks](sys::reactor::timer_token t) {
                    ticks++;
                });

        while(ticks < 3) r.run_one(LONG);

        ok("a repeating timer repeats", ticks >= 3, std::to_string(ticks));
    }

    {
        sys::reactor r;

        int ticks = 0;

        r.every(std::chrono::milliseconds(10),
                [&r, &ticks](sys::reactor::timer_token t) {
                    ticks++;
                    r.cancel(t);
                });

        r.run_one(LONG);
        r.run_one(std::chrono::milliseconds(120));

        ok("and one that cancels itself stops after a tick", ticks == 1,
           std::to_string(ticks));
    }
}

/** Ready descriptors are dispatched before timers due in the same pass. */
static void io_before_timers() {
    sys::reactor r;
    sys::pipe p(false, false);

    std::vector<std::string> order;

    r.add(p.get_reader(), sys::reactor::READ,
          [&order](sys::reactor::token, int, sys::reactor::event_type) {
              order.push_back("io");
          });

    r.after(std::chrono::nanoseconds::zero(),
            [&order](sys::reactor::timer_token) { order.push_back("timer"); });

    p.write_int(1);

    r.run_one(LONG);

    ok("both ran in one pass", order.size() == 2,
       std::to_string(order.size()));

    ok("and the descriptor went first",
       order.size() == 2 && order[0] == "io" && order[1] == "timer");
}

/**
 * HANGUP does not mean there is nothing to read.
 *
 * A peer that sent data and closed gives HANGUP with the data still waiting.
 * A callback that reads HANGUP as end of stream loses the last read, which is
 * the mistake basic_tlsbuf::underflow made one layer down.
 */
static void hangup_is_not_end_of_data() {
    sys::reactor r;

    int fds[2];

    if(::pipe(fds) != 0) {
        std::cout << "  skip  pipe() failed [" << backend << "]\n";
        return;
    }

    const char* payload = "abc";

    feed(fds[1], payload, 3);
    ::close(fds[1]);

    sys::reactor::event_type saw = sys::reactor::NONE;
    int got = 0;
    char buf[8];

    r.add(fds[0], sys::reactor::READ,
          [&](sys::reactor::token t, int fd, sys::reactor::event_type e) {
              saw = e;
              got = int(::read(fd, buf, sizeof buf));
              r.remove(t);
          });

    r.run_one(LONG);

    ok("the closed-with-data descriptor dispatches",
       saw != sys::reactor::NONE, std::to_string(saw));

    ok("and the data is still there to read", got == 3,
       std::to_string(got) + " octets");

    ::close(fds[0]);
}

/** A callback that throws does not end the pass, or the loop. */
static void a_throwing_callback_does_not_stop_the_loop() {
    sys::reactor r;
    sys::pipe p(false, false);

    int reported = 0;
    int after = 0;

    r.on_error([&reported](const std::exception&) { reported++; });

    r.add(p.get_reader(), sys::reactor::READ,
          [&r, &after](sys::reactor::token t, int fd, sys::reactor::event_type) {
              r.remove(t);

              int token;

              // Consumed so the level-triggered registration stops firing;
              // what it was does not matter.
              if(::read(fd, &token, sizeof token) < 0) { /* drained */ }

              r.post([&after]{ after++; });

              throw std::runtime_error("a callback failed");
          });

    p.write_int(1);

    r.run_one(LONG);

    ok("the failure reaches on_error", reported == 1,
       std::to_string(reported));

    r.run_one(LONG);

    ok("and the loop keeps running", after == 1, std::to_string(after));
}

/** A nested pass throws rather than dispatching the same readiness twice. */
static void nesting_throws() {
    sys::reactor r;
    sys::pipe p(false, false);

    bool threw = false;
    int reported = 0;

    r.on_error([&reported](const std::exception&) { reported++; });

    r.add(p.get_reader(), sys::reactor::READ,
          [&](sys::reactor::token t, int fd, sys::reactor::event_type) {
              r.remove(t);

              int token;

              // Consumed so the level-triggered registration stops firing;
              // what it was does not matter.
              if(::read(fd, &token, sizeof token) < 0) { /* drained */ }

              try { r.run_one(SHORT); }
              catch(sys::reactor::exception&) { threw = true; }
          });

    p.write_int(1);

    r.run_one(LONG);

    ok("run_one() from inside a callback throws", threw);

    ok("and the outer pass survived it", reported == 0,
       std::to_string(reported));
}

static void run_every_section() {
    nothing_to_do();
    level_not_edge();
    a_callback_removes_itself();
    a_callback_removes_another();
    an_addition_waits_for_the_next_pass();
    a_reused_descriptor_is_not_the_old_one();
    posting_from_another_thread();
    nothing_posted_is_lost();
    a_posted_remove_beats_the_dispatch();
    stopping();
    timers();
    io_before_timers();
    hangup_is_not_end_of_data();
    a_throwing_callback_does_not_stop_the_loop();
    nesting_throws();
}

int main() {
    std::cout << std::unitbuf;

    std::vector<std::string> backends;

    backends.push_back("poll");

#ifdef HAVE_KQUEUE
    backends.push_back("kqueue");
#endif
#ifdef HAVE_EPOLL
    backends.push_back("epoll");
#endif

    for(std::size_t i = 0; i < backends.size(); i++) {
        backend = backends[i];

        ::setenv("JLIB_SYS_REACTOR", backend.c_str(), 1);

        std::cout << "\n=== " << backend << " ===\n";

        run_every_section();
    }

    // What a green run does not establish.
    //
    // That the backends agree on anything not exercised here.  EV_EOF and
    // POLLHUP have genuinely different shapes on a half-closed socket and only
    // the pipe case above is asserted; a socket with shutdown(SHUT_WR) is not.
    //
    // Anything about scale.  Nothing here registers more than a handful of
    // descriptors, so nothing says the kqueue changelist or the poll array
    // rebuild is the right complexity, and neither has been measured.
    //
    // Thread safety beyond post(), wake() and stop().  add() and remove() from
    // a foreign thread are undefined by contract, so they are not tested --
    // which is not the same as their working.
    //
    // That timers are *accurate*.  Only that they are ordered, that they are
    // never early, and that a repeating one repeats.
    //
    // And the self-removal section may pass on a naive implementation without
    // a sanitizer: the use-after-free it is written to catch is only reliably
    // caught under ASan.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
