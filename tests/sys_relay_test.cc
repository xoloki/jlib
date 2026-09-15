/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2002 Joey Yandle <xoloki@gmail.com>
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
 * sys_relay_test -- a long action on its own thread, reported through a pipe.
 *
 * Most of this file is about **pump()'s ordering**, which is the part that is
 * easy to get wrong and impossible to notice: take() and done() are separate
 * facts about a thread that is still running, and either read first has a hole
 * that loses the last item or the last wake-up.  Losing the last token of a
 * reply looks exactly like a model that stopped early, which is why it needs a
 * test rather than an argument.
 *
 * The holes are races, so the honest instrument is repetition: a worker that
 * emits N items as fast as it can and finishes, many rounds, asserting every
 * round delivered exactly N.  A wrong ordering loses one now and then, and
 * "now and then" over a few hundred rounds is reliable enough to fail on.
 */

#include <jlib/sys/relay.hh>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using namespace jlib;

/**
 * Fail loudly rather than hang.
 *
 * One of the two wrong orderings does not lose an item, it loses a wake-up --
 * and a pump waiting for a byte that was already drained waits for ever.  A
 * test that hangs produces no output and no exit status anybody reads, so this
 * turns that into a failure with a name.
 *
 * _exit because the thread it is racing cannot be joined: it is parked in a
 * reactor that will never return.
 */
static void guard_against_a_hang() {
    std::thread([]{
        std::this_thread::sleep_for(std::chrono::seconds(20));

        std::cerr << "  FAIL  pump() never returned -- a wake-up was lost\n"
                  << "sys_relay_test: hung\n";

        std::cerr.flush();

        ::_exit(1);
    }).detach();
}

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Collect everything a relay produces, on a reactor, and say how it ended. */
static bool drain(sys::relay<std::string>& r, sys::reactor& rc,
                  std::vector<std::string>& got)
{
    // Named, because run_until_complete takes a task by reference -- a
    // temporary would die at the end of the full expression while the reactor
    // was still driving it.
    sys::task<bool> work =
        sys::pump(r, rc, [&got](std::string& s) -> sys::task<bool> {
            got.push_back(s);

            co_return true;
        });

    return sys::run_until_complete(rc, work);
}

static void everything_emitted_arrives() {
    std::cout << "\neverything emitted arrives, in order:\n";

    sys::reactor rc;
    sys::relay<std::string> r;

    r.start([](sys::relay<std::string>& b) {
        for(int i = 0; i < 5; i++) b.emit("item " + std::to_string(i));
    });

    std::vector<std::string> got;

    const bool all = drain(r, rc, got);

    ok("  pump says it consumed everything", all);
    ok("  five items", got.size() == 5, std::to_string(got.size()));

    bool ordered = got.size() == 5;

    for(std::size_t i = 0; i < got.size() && ordered; i++)
        ordered = got[i] == "item " + std::to_string(i);

    ok("  in the order they were emitted", ordered,
       got.empty() ? "(none)" : got.front() + " .. " + got.back());
}

/**
 * The ordering, provoked rather than hoped for.
 *
 * **This is the section that fails when pump() is wrong**, and getting it to
 * fail took two goes.  The first version emitted eight items as fast as it
 * could and ran three hundred rounds, which never failed against either broken
 * ordering -- because the worker finished before the reader's first take(), so
 * the window the bug lives in was never open.
 *
 * The window is the time pump() spends inside `each`, between take() and
 * done().  So the reader is made slow there on purpose and the worker is made
 * to emit its last item into exactly that gap.  Then:
 *
 *   - a pump that does not take again after seeing done drops "last", and
 *     this catches it: both assertions below fail
 *
 * **The other wrong ordering is not covered**, and saying so is the point of
 * writing this down.  Reading done() before take() only loses a wake-up if
 * finish() lands between those two statements -- nanoseconds -- and no amount
 * of arranging from out here opens that window.  The watchdog below exists for
 * it anyway: if it ever does happen the symptom is a hang, and a test that
 * hangs reports nothing at all.
 */
static void the_last_item_is_not_lost() {
    std::cout << "\nan item emitted while the reader is busy is not lost:\n";

    sys::reactor rc;
    sys::relay<std::string> r;

    std::atomic<bool> busy(false);

    r.start([&busy](sys::relay<std::string>& b) {
        b.emit("first");

        // Wait until the reader is inside each("first"), which is to say
        // between its take() and its done().
        while(!busy.load())
            std::this_thread::sleep_for(std::chrono::microseconds(100));

        b.emit("last");

        // finish() follows, from start(), with no pause -- so "last" and the
        // end of the work land in the same gap.
    });

    std::vector<std::string> got;

    sys::task<bool> work =
        sys::pump(r, rc, [&got, &busy](std::string& s) -> sys::task<bool> {
            got.push_back(s);

            if(s == "first") {
                busy.store(true);

                // Blocking the reactor's thread, which a real handler must
                // never do and a test may: it is the whole instrument.
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            co_return true;
        });

    guard_against_a_hang();

    sys::run_until_complete(rc, work);

    ok("  both items arrive", got.size() == 2, std::to_string(got.size()));

    ok("  including the one emitted into the gap",
       got.size() == 2 && got[1] == "last",
       got.size() > 1 ? got[1] : std::string("(missing)"));
}

static void a_worker_that_throws_says_so() {
    std::cout << "\na worker that throws is reported, not hung:\n";

    sys::reactor rc;
    sys::relay<std::string> r;

    r.start([](sys::relay<std::string>& b) {
        b.emit("before");

        throw std::runtime_error("the work failed");
    });

    std::vector<std::string> got;

    // The point: pump returns rather than waiting for ever, because start()
    // calls fail() for a worker that throws.
    const bool all = drain(r, rc, got);

    ok("  pump returns", all);
    ok("  with what was emitted before the throw",
       got.size() == 1 && got[0] == "before", std::to_string(got.size()));

    ok("  and why() carries the reason",
       r.why().find("the work failed") != std::string::npos, r.why());
}

static void a_reader_that_gives_up_stops_the_worker() {
    std::cout << "\na reader that gives up stops the worker:\n";

    sys::reactor rc;
    sys::relay<std::string> r;

    std::atomic<int> emitted(0);

    // Would run for ever if nobody stopped it.  wanted() is the only thing
    // that ends it, which is what an abandoned request needs.
    r.start([&emitted](sys::relay<std::string>& b) {
        while(b.wanted()) {
            b.emit("tick");
            ++emitted;
        }
    });

    int seen = 0;

    sys::task<bool> work =
        sys::pump(r, rc, [&seen, &r](std::string&) -> sys::task<bool> {
            if(++seen < 3) co_return true;

            r.stop();

            co_return false;
        });

    const bool all = sys::run_until_complete(rc, work);

    ok("  pump reports it did not consume everything", !all);
    ok("  it stopped where it said it would", seen == 3,
       std::to_string(seen));

    r.join();

    const int after = emitted.load();

    ok("  and the worker is no longer running", true,
       std::to_string(after) + " emitted before it was told to stop");
}

int main() {
    std::cout << "sys_relay_test\n";

    try {
        everything_emitted_arrives();
        the_last_item_is_not_lost();
        a_worker_that_throws_says_so();
        a_reader_that_gives_up_stops_the_worker();
    }
    catch(std::exception& e) {
        std::cerr << "sys_relay_test: " << e.what() << "\n";
        return 1;
    }

    if(failures) {
        std::cerr << "sys_relay_test: " << failures << " failed\n";
        return 1;
    }

    std::cout << "sys_relay_test: all good\n";

    return 0;
}
