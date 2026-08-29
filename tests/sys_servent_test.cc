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

// When Servent's worker sleeps and when it ticks.
//
// The class dates from 2002 and had no test.  It is really two things sharing
// a name -- a command dispatcher, which has nothing to do between commands,
// and a periodic-work runner, which must keep going round -- and every
// assertion here is about it being the right one at the right time.

#include <jlib/sys/Servent.hh>

#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace sys = jlib::sys;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** CPU this process has burned, user plus system. */
static double cpu_seconds() {
    struct rusage ru;

    if(::getrusage(RUSAGE_SELF, &ru) != 0) return -1;

    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 +
           ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

static double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

template<typename Predicate>
static bool within(double seconds, Predicate pred) {
    const auto start = std::chrono::steady_clock::now();

    while(seconds_since(start) < seconds) {
        if(pred()) return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    return pred();
}

/** A condition that counts how often it was asked, and never fires. */
static sys::Servent::condition_list_type::value_type
counting_condition(std::atomic<int>& asked) {
    return std::make_pair(
        std::function<bool()>([&asked]() -> bool { asked++; return false; }),
        std::function<void()>([] {}));
}

static void a_command_reaches_its_slot() {
    std::cout << "\na command reaches its slot:\n";

    sys::Servent s;

    std::atomic<int> ran{0};

    s.map(1, [&ran] { ran++; });

    s.run();

    s.exec(1);

    ok("a mapped command runs", within(5, [&ran] { return ran.load() == 1; }),
       std::to_string(ran.load()));

    s.exec(1);

    ok("and again", within(5, [&ran] { return ran.load() == 2; }),
       std::to_string(ran.load()));

    s.stop();

    ok("and stop() returns, from a worker that was blocked in poll", true);
}

static void an_idle_servent_costs_nothing() {
    std::cout << "\nan idle servent costs nothing:\n";

    sys::Servent s;

    std::atomic<int> ran{0};

    // A command mapped but never sent: this is the dispatcher case, with
    // nothing periodic registered, which is the one that should sleep.
    s.map(1, [&ran] { ran++; });

    s.run();

    // Sleep rather than within(): within() polls on *this* thread, and this
    // section measures the process.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const double before = cpu_seconds();

    std::this_thread::sleep_for(std::chrono::seconds(3));

    const double used = cpu_seconds() - before;

    // The old loop polled the command pipe on a 1ms timeout whether or not
    // anyone had asked for a tick, and cost ~13ms of CPU per idle second.
    // Blocking is below what getrusage resolves.  See sys_asservent_test for
    // where this threshold came from -- the case it has to catch is not the
    // 1ms poll but the cheaper 1ms *timed wait* somebody might write instead.
    ok("three idle seconds burn no measurable CPU", used >= 0 && used < 0.005,
       std::to_string(used) + "s over 3s idle");

    s.exec(1);

    ok("and the worker is still there to be woken",
       within(5, [&ran] { return ran.load() == 1; }));

    s.stop();
}

static void a_registered_condition_still_ticks() {
    std::cout << "\na registered condition still ticks:\n";

    sys::Servent s;

    std::atomic<int> asked{0};

    // Registered before run(), so the worker is in polling mode from the
    // start.  This is the behaviour the branch must not break: somebody who
    // asks for a predicate to be evaluated on every pass has to keep getting
    // that, whatever it costs.
    s.add(counting_condition(asked));

    s.run();

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int n = asked.load();

    s.stop();

    // At 1ms a pass, 300ms is on the order of 300 evaluations.  Asserting
    // "more than 50" rather than a real figure, because the point is that it
    // is polling at all and the exact rate is the scheduler's business.
    ok("the predicate is evaluated over and over", n > 50,
       std::to_string(n) + " evaluations in 300ms");
}

static void adding_a_condition_wakes_a_sleeping_worker() {
    std::cout << "\nadding a condition wakes a sleeping worker:\n";

    sys::Servent s;

    std::atomic<int> ran{0};
    std::atomic<int> knocks{0};

    s.map(1, [&ran] { ran++; });

    // Mapping a reserved id, which the header tells callers not to do.  The
    // test does it because it is the only way to see, from out here, whether
    // the knock reaches the dispatcher: WAKE has to end the poll and then be
    // dropped, and a WAKE that fell through to the command map would both
    // report an unmapped command and -- if one were mapped, as here -- run it.
    // Capturing stderr would show the first, but swapping cerr's buffer under
    // a live worker is a data race, and this is not.
    s.map(sys::Servent::WAKE, [&knocks] { knocks++; });

    s.run();

    // Long enough that the worker is certainly asleep in poll(-1): nothing
    // periodic is registered yet.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::atomic<int> asked{0};

    s.add(counting_condition(asked));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int n = asked.load();

    // The direct test of WAKE.  Without the knock inside add(), the worker
    // stays in poll(-1) -- it decided to block back when there was nothing to
    // evaluate, and nothing else is going to arrive -- so this reads zero and
    // the condition is never evaluated at all.
    ok("a condition added to a sleeping worker starts being evaluated", n > 50,
       std::to_string(n) + " evaluations in 300ms");

    ok("and the knock was never dispatched as a command", knocks.load() == 0,
       std::to_string(knocks.load()) + " dispatches of WAKE");

    // And the knock did not break dispatch either.
    s.exec(1);

    ok("and a real command still dispatches afterwards",
       within(5, [&ran] { return ran.load() == 1; }),
       std::to_string(ran.load()));

    s.stop();
}

static void connecting_cycle_is_asking_for_a_tick() {
    std::cout << "\nconnecting cycle is asking for a tick:\n";

    sys::Servent s;

    std::atomic<int> cycles{0};

    // Connected before run(), which is the documented order: cycle is a
    // public member, so this class cannot see the connection happen and
    // cannot knock on its own behalf.
    s.cycle.connect([&cycles] { cycles++; });

    s.run();

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const int n = cycles.load();

    s.stop();

    ok("cycle emits over and over", n > 50,
       std::to_string(n) + " emissions in 300ms");
}

int main() {
    std::cout << std::unitbuf;

    a_command_reaches_its_slot();
    an_idle_servent_costs_nothing();
    a_registered_condition_still_ticks();
    adding_a_condition_wakes_a_sleeping_worker();
    connecting_cycle_is_asking_for_a_tick();

    // What a green run does not establish.
    //
    // That connecting to cycle *after* run() works.  It does not, and that is
    // the one asymmetry in the design: add() knocks for itself, but cycle is a
    // public member and connecting to it goes through no method of this class,
    // so a worker already asleep stays asleep.  wake() is the manual knock and
    // the header says so.  Nothing here tests the broken order, because
    // asserting a documented limitation reads like a bug report.
    //
    // Not the polling rate.  1ms is still hardcoded, so a caller who wants a
    // tick pays the old cost exactly -- this branch chose when to poll, not
    // how often, and making the interval a parameter is a separate question.
    //
    // Not that an unguarded WAKE would be *reported*.  The assertion above
    // catches it running a mapped slot, which is the harmful half; the "cannot
    // find signal" line it would also print on every add() goes to stderr, and
    // reading that would mean swapping cerr's buffer while the worker writes
    // to it, which is a data race in the test rather than a check of the code.
    //
    // Not the pipe-failure path.  A broken command pipe now stops the loop
    // rather than spinning on it, which matters more with a blocking poll than
    // it did with a 1ms one, and there is no way to break a pipe under a live
    // worker from out here without racing it.
    //
    // Not media::Player, which is the only subclass and is covered by
    // media_player_test rather than by anything here.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
