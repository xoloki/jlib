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

// sync<T>, and the job queue built out of one.
//
// This file used to construct a sync<int>, assign 1 to it, and exit -- which is
// why the pool in the same header went years with a bug that made it a pool of
// threads running one job at a time.  The assertion that matters below is the
// one that counts how many jobs are in flight at once: it is the only one that
// cannot pass merely because the code compiles.

#include <jlib/sys/sync.hh>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Qualified, never hoisted with a using-declaration.  "using jlib::sys::sync;"
// at namespace scope collides with POSIX sync(2), which glibc drags in through
// <thread>, and the error gcc gives for it -- "expected '(' after
// template-argument-list" on the line below -- names neither <unistd.h> nor the
// collision.  sys/signal.hh has the same note about ::signal, one name over.
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

/** Records the greatest number of jobs that were ever running at once. */
class watcher {
public:
    void enter() {
        const int now = ++m_live;

        int seen = m_peak.load();

        while(now > seen && !m_peak.compare_exchange_weak(seen, now))
            ;
    }

    void leave() { m_live--; m_done++; }

    int peak() const { return m_peak.load(); }
    int done() const { return m_done.load(); }

private:
    std::atomic<int> m_live{0};
    std::atomic<int> m_peak{0};
    std::atomic<int> m_done{0};
};

static void a_value_behind_a_mutex() {
    std::cout << "a value behind a mutex:\n";

    sys::sync<int> i(0);

    ok("it starts where it was put", i == 0, std::to_string(static_cast<int>(i)));

    i = 1;

    ok("and holds what it is given", i == 1, std::to_string(static_cast<int>(i)));

    i.set(2);

    ok("set() and get() agree with the operators", i.get() == 2 && i == 2,
       std::to_string(i.get()));

    // A reader on another thread, woken by the writer.  wait() is what the job
    // queue in the same header is built on.
    sys::sync<bool> flag(false);

    std::thread writer([&flag] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        flag.set(true);
    });

    {
        std::unique_lock<std::mutex> lock(flag.mutex());

        while(!flag()) flag.wait(lock);
    }

    writer.join();

    ok("a waiter is woken by a set()", flag.get());
}

static void jobs_run_on_more_than_one_thread() {
    std::cout << "\njobs run on more than one thread:\n";

    // The headline.  Four workers, eight jobs of 100ms each.  Holding the lock
    // while the job ran -- which is what this did -- makes the peak 1 and the
    // whole thing take eight tenths of a second; releasing it first makes the
    // peak up to four and the whole thing take about two.
    watcher w;

    const auto start = std::chrono::steady_clock::now();

    {
        sys::job_queue q(4);

        for(int i = 0; i < 8; i++) {
            q.post([&w] {
                w.enter();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                w.leave();
            });
        }

        // The destructor drains and joins.
    }

    const double took = seconds_since(start);

    ok("more than one job ran at once", w.peak() > 1,
       "peak " + std::to_string(w.peak()));

    ok("and never more than the pool", w.peak() <= 4,
       "peak " + std::to_string(w.peak()));

    // Written as a comparison rather than a number: a busy machine makes any
    // absolute figure a lie, and this is the shape of the claim anyway.
    ok("so it finished in less time than running them one at a time would take",
       took < 0.8, std::to_string(took) + "s");

    ok("every job ran", w.done() == 8, std::to_string(w.done()) + "/8");
}

static void the_destructor_drains() {
    std::cout << "\nthe destructor drains:\n";

    std::atomic<int> ran{0};

    {
        sys::job_queue q(2);

        for(int i = 0; i < 20; i++) q.post([&ran] { ran++; });
    }

    // A queue going out of scope with work outstanding must not silently
    // discard it -- ~job_queue() is stop() with drain, then join().
    ok("everything posted before the queue went away had run", ran.load() == 20,
       std::to_string(ran.load()) + "/20");
}

static void stopping_with_and_without_a_drain() {
    std::cout << "\nstopping, with and without a drain:\n";

    {
        std::atomic<int> ran{0};

        sys::job_queue q(2);

        for(int i = 0; i < 20; i++) q.post([&ran] { ran++; });

        q.stop();
        q.join();

        ok("stop() runs what was already queued", ran.load() == 20,
           std::to_string(ran.load()) + "/20");
    }

    {
        std::atomic<int> ran{0};

        // One worker and slow jobs, so there is plenty still queued when the
        // stop lands.
        sys::job_queue q(1);

        for(int i = 0; i < 20; i++) {
            q.post([&ran] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                ran++;
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const auto start = std::chrono::steady_clock::now();

        q.stop(false);
        q.join();

        const double took = seconds_since(start);

        ok("stop(false) abandons what is left", ran.load() < 20,
           std::to_string(ran.load()) + "/20");

        // Bounded by the job in hand, not by everything queued: twenty of
        // these would be four tenths of a second.
        ok("and returns within one job", took < 0.2,
           std::to_string(took) + "s");
    }

    {
        sys::job_queue q(2);

        std::atomic<int> ran{0};

        q.stop();

        // Ignored rather than queued: it would never run, and returning
        // quietly having queued it would make that indistinguishable from
        // having run it.
        q.post([&ran] { ran++; });

        q.join();

        ok("a job posted after stop() is dropped", ran.load() == 0,
           std::to_string(ran.load()));

        // Both are idempotent, which the destructor relies on: it calls them
        // again after this.
        q.stop();
        q.stop(false);
        q.join();
        q.join();

        ok("and stop() and join() can be called twice", true);
    }
}

static void a_job_that_throws() {
    std::cout << "\na job that throws:\n";

    // An exception escaping a thread function is std::terminate, and it cannot
    // be caught from outside -- exceptions do not cross thread boundaries.  So
    // the catch has to be where the job is called, and a pool that runs
    // arbitrary functors will be handed a throwing one eventually.
    std::atomic<int> reported{0};
    std::atomic<int> after{0};

    {
        sys::job_queue q(2);

        q.on_error([&reported](const std::exception&) { reported++; });

        q.post([] { throw std::runtime_error("no"); });

        // Give the throw a moment to land before the next job, so "the worker
        // survived" is about the worker and not about the other one.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        for(int i = 0; i < 4; i++) q.post([&after] { after++; });
    }

    ok("the process is still here", true);
    ok("the handler was told", reported.load() == 1,
       std::to_string(reported.load()));
    ok("and the pool went on taking jobs", after.load() == 4,
       std::to_string(after.load()) + "/4");
}

static void a_pool_of_none_is_a_pool_of_one() {
    std::cout << "\na pool of none is a pool of one:\n";

    // std::thread::hardware_concurrency() is permitted to return 0, and it is
    // the default argument -- so a queue that accepted jobs and ran none was
    // one unlucky platform away.
    std::atomic<int> ran{0};

    {
        sys::job_queue q(0);

        for(int i = 0; i < 5; i++) q.post([&ran] { ran++; });
    }

    ok("jobs still run", ran.load() == 5, std::to_string(ran.load()) + "/5");
}

int main() {
    std::cout << std::unitbuf;

    a_value_behind_a_mutex();
    jobs_run_on_more_than_one_thread();
    the_destructor_drains();
    stopping_with_and_without_a_drain();
    a_job_that_throws();
    a_pool_of_none_is_a_pool_of_one();

    // What a green run does not establish.
    //
    // Not the absence of the lost wakeup this fixed.  A worker used to test
    // m_exit outside the lock and then wait, so a stop() landing in between
    // was a notification it never saw and a join() that never returned -- a
    // window a few instructions wide.  What the predicate loop buys is that
    // the race cannot happen; no timing test can show it gone, and one that
    // claimed to would be worse than this paragraph.
    //
    // Not the timings.  "Peak greater than one" is robust and is the real
    // assertion; "finished faster than serial would" has a wide margin and is
    // still a wall-clock claim on a machine that may be busy.  A failure there
    // means look at the machine before looking at the code.
    //
    // Not sync<T> in anger.  Two threads and one condition variable is the
    // whole of what is exercised above; the ringbuffer test is where the
    // memory-ordering questions in this library actually live, and it says the
    // same thing about itself.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
