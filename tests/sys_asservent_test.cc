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

// Who owns ASServent's worker thread.
//
// The class has been in the tree since 2002 and had no test at all, which is
// how reset() came to drop a std::thread without joining it and how the
// destructor came to be empty.  Everything here is about the thread's
// lifetime; the request and response *plumbing* is exercised only as far as
// it takes to prove which thread is running.

#include <jlib/sys/ASServent.hh>

#include <chrono>
#include <iostream>
#include <mutex>
#include <set>
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

/** Request and response have to be distinct types: handle() is overloaded. */
struct job {
    int n = 0;

    // ASServent queues requests by priority, so Request needs this.
    bool operator<(const job& o) const { return n < o.n; }
};

struct answer {
    int n = 0;
};

/**
 * Records what ran and, more to the point, *where*.
 *
 * The thread id is the whole test: a leaked worker is not visible as a leak
 * from in here, but it is visible as a second id serving requests after a
 * reset() that was supposed to have retired it.
 */
class counter : public sys::ASServent<job, answer> {
public:
    virtual ~counter() {
        // The contract ASServent::stop() documents.  Without this the base
        // destructor would still join, but only after m_ids and m_served --
        // which handle() touches -- had already been destroyed.
        stop();
    }

    virtual void handle(const job& j) {
        std::lock_guard<std::mutex> lock(m_lock);

        m_ids.insert(std::this_thread::get_id());
        m_served.push_back(j.n);
    }

    virtual void handle(const answer&) {}

    std::size_t handled() const {
        std::lock_guard<std::mutex> lock(m_lock);

        return m_served.size();
    }

    std::set<std::thread::id> ids() const {
        std::lock_guard<std::mutex> lock(m_lock);

        return m_ids;
    }

    std::vector<int> served() const {
        std::lock_guard<std::mutex> lock(m_lock);

        return m_served;
    }

    void forget() {
        std::lock_guard<std::mutex> lock(m_lock);

        m_ids.clear();
        m_served.clear();
    }

private:
    mutable std::mutex m_lock;
    std::set<std::thread::id> m_ids;
    std::vector<int> m_served;
};

/** Poll a predicate rather than sleeping a guessed interval. */
template<typename Predicate>
static bool within(double seconds, Predicate pred) {
    const auto start = std::chrono::steady_clock::now();

    while(seconds_since(start) < seconds) {
        if(pred()) return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    return pred();
}

static void a_worker_serves_what_is_pushed() {
    std::cout << "\na worker serves what is pushed:\n";

    counter c;

    ok("stop() on one that was never run is harmless", (c.stop(), true));

    c.run();

    for(int i = 0; i < 5; i++) c.push(job{i});

    ok("every request reaches the worker", within(5, [&c] { return c.handled() == 5; }),
       std::to_string(c.handled()) + "/5");

    const std::set<std::thread::id> ids = c.ids();

    ok("on exactly one thread", ids.size() == 1, std::to_string(ids.size()));

    ok("and not on this one",
       ids.size() == 1 && *ids.begin() != std::this_thread::get_id());
}

static void reset_retires_the_worker_it_replaces() {
    std::cout << "\nreset() retires the worker it replaces:\n";

    counter c;

    c.run();

    c.push(job{0});

    ok("the first worker serves", within(5, [&c] { return c.handled() == 1; }));

    // Five rounds, because the old failure was a race rather than a certainty:
    // reset() wrote EXIT and spawned a replacement without joining, so both
    // workers were reading the same pipe and *either* could take the byte.  If
    // the new one took it, it exited immediately and the requests below were
    // never served at all.
    const int rounds = 5;
    const int each = 10;

    for(int round = 0; round < rounds; round++) {
        c.forget();
        c.reset();

        for(int i = 0; i < each; i++) c.push(job{i});

        const bool all = within(5, [&c, each] { return c.handled() == std::size_t(each); });

        ok("round " + std::to_string(round + 1) + " is served by the new worker", all,
           std::to_string(c.handled()) + "/" + std::to_string(each));

        const std::set<std::thread::id> ids = c.ids();

        // The direct assertion.  Two ids here means the worker reset() was
        // supposed to have retired is still alive and still taking requests.
        ok("  and by one worker only", ids.size() == 1, std::to_string(ids.size()));
    }
}

static void the_destructor_waits_for_the_worker() {
    std::cout << "\nthe destructor waits for the worker:\n";

    std::chrono::steady_clock::time_point start;

    {
        counter c;

        c.run();

        c.push(job{1});

        ok("the worker is running", within(5, [&c] { return c.handled() == 1; }));

        // Last statement in the scope, so what follows measures the
        // destructor and nothing else.
        start = std::chrono::steady_clock::now();
    }

    const double took = seconds_since(start);

    // The assertion is that this returns at all.  Before the fix it returned
    // instantly and left the thread running on a destroyed object; a
    // destructor that joined without asking the loop to leave would never
    // return.  Both failures are visible here, at opposite ends.
    ok("it returns, and promptly", took < 2.0, std::to_string(took) + "s");
}

static void stopping_twice_is_allowed() {
    std::cout << "\nstopping twice is allowed:\n";

    counter c;

    c.run();

    c.push(job{1});

    within(5, [&c] { return c.handled() == 1; });

    c.stop();

    const std::size_t after_first = c.handled();

    ok("stop() ends the worker", after_first == 1, std::to_string(after_first));

    c.stop();

    ok("and a second stop() is a no-op rather than a hang", true);

    // A queue with no worker: accepted, and served by whoever runs next.
    c.push(job{2});

    ok("a request pushed with no worker is not served",
       !within(0.3, [&c] { return c.handled() == 2; }),
       std::to_string(c.handled()));

    c.reset();

    ok("and is picked up when one is started again",
       within(5, [&c] { return c.handled() == 2; }),
       std::to_string(c.handled()));
}

int main() {
    std::cout << std::unitbuf;

    a_worker_serves_what_is_pushed();
    reset_retires_the_worker_it_replaces();
    the_destructor_waits_for_the_worker();
    stopping_twice_is_allowed();

    // What a green run does not establish.
    //
    // That no thread outlives its ASServent.  Nothing here sees a leaked
    // worker directly; it is inferred from a *second* thread id serving after
    // a reset that should have retired the first, which is evidence and not
    // proof.  A sanitizer build is what would show the use-after-free, and the
    // structural guarantee is that m_worker is now a std::thread rather than a
    // raw pointer, so assigning over a joinable one calls std::terminate
    // instead of leaking quietly.
    //
    // In particular this cannot count workers by their ids.  An id may be
    // reused once its thread has ended, and in practice every round of
    // reset() here gets the same one back -- which is consistent with the old
    // worker having been joined first, and is also exactly what the standard
    // permits an implementation to do for any reason.  So the per-round
    // "one worker only" is the assertion; a tally across rounds would be
    // testing the allocator.
    //
    // Not the pure virtual call the base destructor still permits.  ~ASServent
    // joins, but a subclass that does not stop() in its own destructor can
    // still have its worker inside handle() when the derived part is gone.
    // Every subclass here and in net/ does stop(); nothing enforces it.
    //
    // Not the response half.  push(Response) and the handle() pump on the
    // response pipe are untouched by this branch and untested by this file.
    //
    // Not the stop() fallback path, where write_int(EXIT) throws on a full
    // pipe and m_bunny is what ends the loop.  Filling a pipe that only the
    // worker reads means racing the worker, and a test that has to win a race
    // to pass is worse than the comment saying it is untested.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
