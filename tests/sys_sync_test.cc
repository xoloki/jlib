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

    // The predicate form, which is the one to reach for: the condition is
    // stated once, positively, and re-checked under the lock every time, so
    // there is no loop to forget.
    sys::sync<int> counter(0);

    std::thread counting([&counter] {
        for(int n = 1; n <= 3; n++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            counter.set(n);
        }
    });

    {
        std::unique_lock<std::mutex> lock(counter.mutex());

        counter.wait(lock, [&counter] { return counter() == 3; });

        ok("wait() with a predicate returns when it holds", counter() == 3,
           std::to_string(counter()));
    }

    counting.join();

    // And the timed overload still resolves, which is not a formality.  An
    // unconstrained predicate template would swallow it: milliseconds needs a
    // converting constructor to reach nanoseconds -- a user-defined conversion
    // -- while the template matches it exactly, and an exact template match
    // beats a converting non-template.  The call would bind to the predicate
    // overload and fail to compile inside, trying to call a duration.  This
    // line is what keeps the requires-clause from being tidied away.
    {
        sys::sync<int> quiet(0);

        std::unique_lock<std::mutex> lock(quiet.mutex());

        const auto start = std::chrono::steady_clock::now();

        quiet.wait(lock, std::chrono::milliseconds(20));

        ok("and a duration still picks the timed overload, not this one",
           seconds_since(start) >= 0.01, std::to_string(seconds_since(start)) + "s");
    }
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

static void a_pool_of_none_runs_inline() {
    std::cout << "\na pool of none runs inline:\n";

    // Not a degenerate case but a mode: with no pool, post() runs the job on
    // the calling thread.  Which is the point -- a caller that always posts can
    // be made synchronous or concurrent by one number, and does not carry two
    // code paths to choose between.
    std::atomic<int> ran{0};
    std::thread::id where;

    {
        sys::job_queue q(0);

        for(int i = 0; i < 5; i++) {
            q.post([&ran, &where] {
                where = std::this_thread::get_id();
                ran++;
            });
        }

        // Already, before anything is stopped or joined: post() ran it.
        ok("the job has run by the time post() returns", ran.load() == 5,
           std::to_string(ran.load()) + "/5");

        ok("and it ran on the thread that posted it",
           where == std::this_thread::get_id());
    }

    // The same job on a pool runs somewhere else, which is the contrast that
    // makes the assertion above mean something.
    std::thread::id elsewhere;

    {
        sys::job_queue q(1);

        q.post([&elsewhere] { elsewhere = std::this_thread::get_id(); });
    }

    ok("where a pool of one does not", elsewhere != std::this_thread::get_id());

    // Everything else is the same, which is what lets a caller ignore the
    // difference: a job that throws is still caught and handed to on_error,
    // and post() does not throw it at whoever called.
    std::atomic<int> reported{0};
    bool post_threw = false;

    {
        sys::job_queue q(0);

        q.on_error([&reported](const std::exception&) { reported++; });

        try { q.post([] { throw std::runtime_error("no"); }); }
        catch(...) { post_threw = true; }
    }

    ok("a throwing job is caught inline too", reported.load() == 1,
       std::to_string(reported.load()));
    ok("and post() does not throw it at the caller", !post_threw);

    // And a stopped queue drops the job rather than running it here.
    std::atomic<int> after{0};

    {
        sys::job_queue q(0);

        q.stop();
        q.post([&after] { after++; });
    }

    ok("a job posted after stop() is dropped, pool or no pool",
       after.load() == 0, std::to_string(after.load()));
}

int main() {
    std::cout << std::unitbuf;

    a_value_behind_a_mutex();
    jobs_run_on_more_than_one_thread();
    the_destructor_drains();
    stopping_with_and_without_a_drain();
    a_job_that_throws();
    a_pool_of_none_runs_inline();

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
    // Not sync<T> in anger.  A handful of threads and one condition variable
    // is the whole of what is exercised above; the ringbuffer test is where the
    // memory-ordering questions in this library actually live, and it says the
    // same thing about itself.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
