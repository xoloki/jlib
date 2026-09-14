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
#include <jlib/sys/pipe.hh>
#include <jlib/sys/reactor.hh>

#include <sys/resource.h>

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
 * Answers every request, so there is something to announce down the response
 * pipe.  `counter` deliberately does not: its subject is the request half.
 */
class echoer : public sys::ASServent<job, answer> {
public:
    // Or the two overloads below hide the base's no-argument handle(), which
    // is the one an event loop is told to call.  ASMailBox needed the same
    // line and did not have it; see the note there.
    using sys::ASServent<job, answer>::handle;

    virtual ~echoer() { stop(); }

    virtual void handle(const job& j) {
        answer a;

        a.n = j.n;

        push(a);
    }

    virtual void handle(const answer& a) {
        std::lock_guard<std::mutex> lock(m_lock);

        m_answers.push_back(a.n);
        m_on.insert(std::this_thread::get_id());
    }

    std::vector<int> answers() const {
        std::lock_guard<std::mutex> lock(m_lock);

        return m_answers;
    }

    std::set<std::thread::id> answered_on() const {
        std::lock_guard<std::mutex> lock(m_lock);

        return m_on;
    }

private:
    mutable std::mutex m_lock;
    std::vector<int> m_answers;
    std::set<std::thread::id> m_on;
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

static void an_idle_worker_costs_nothing() {
    std::cout << "\nan idle worker costs nothing:\n";

    counter c;

    c.run();

    c.push(job{1});

    ok("the worker is running", within(5, [&c] { return c.handled() == 1; }));

    // Sleep rather than within(): within() polls on *this* thread, and this
    // section measures the process, so the test's own spinning would be
    // indistinguishable from the worker's.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const double before = cpu_seconds();

    std::this_thread::sleep_for(std::chrono::seconds(3));

    const double used = cpu_seconds() - before;

    // Process-wide CPU, but the only two threads are this one -- asleep in
    // nanosleep -- and the worker, which should be blocked on a condition
    // variable.  Three measurements set the threshold:
    //
    //   0.040s  the old loop, polling the request pipe on a 1ms timeout
    //   0.021s  a 1ms *timed* wait on the condition variable, which is the
    //           cheaper way to write the same mistake and so the one the
    //           threshold has to catch
    //   0.00004s  blocking, which is below what getrusage can resolve
    //
    // 5ms sits four times under the cheapest spin and a hundred times over
    // the real figure.  An earlier 20ms threshold passed the middle case by
    // 4%, which is not a threshold, it is a coin toss.
    ok("three idle seconds burn no measurable CPU", used >= 0 && used < 0.005,
       std::to_string(used) + "s over 3s idle");

    ok("and the worker is still there afterwards",
       (c.push(job{2}), within(5, [&c] { return c.handled() == 2; })));
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

/**
 * One thread waiting on a request and a descriptor at once.
 *
 * This is what #4 was for, and until the reactor existed jlib could not do it.
 * get_response_reader() has been a published contract since 2002 -- the header
 * explains at length that the response half is a pipe *precisely* so a foreign
 * event loop can select on it -- and **nothing in this tree has ever called
 * it**. The GUI that consumed it lives outside the repo, so the contract has
 * been carried for twenty-four years with nothing verifying it.
 *
 * Pinned here without changing a line of ASServent.hh: the reactor is the
 * event loop the header always assumed somebody else would bring.
 */
static void one_loop_waits_on_both() {
    std::cout << "\none loop waits on a response and a descriptor:\n";

    echoer e;
    sys::reactor r;
    sys::pipe other(false, false);

    // reset(), not start().  start() *is* the worker loop and blocks forever;
    // reset() is what puts it on a thread.  The header says "Blocks." and the
    // name does not, which cost a hang to find out.
    e.reset();

    int answered = 0;
    int elsewhere = 0;

    std::set<std::thread::id> dispatched_on;

    r.add(e.get_response_reader(), sys::reactor::READ,
          [&](sys::reactor::token, int, sys::reactor::event_type) {
              dispatched_on.insert(std::this_thread::get_id());

              // Reads the byte and drains the response queue, which is exactly
              // what the header tells an event loop to do here.
              e.handle();

              answered++;
          });

    r.add(other.get_reader(), sys::reactor::READ,
          [&](sys::reactor::token, int, sys::reactor::event_type) {
              dispatched_on.insert(std::this_thread::get_id());

              other.read_int();

              elsewhere++;
          });

    ok("  both sources are registered on one reactor", r.count() == 2,
       std::to_string(r.count()));

    job j;

    j.n = 7;

    e.push(j);

    other.write_int(1);

    // One thread, both sources.  Before this there was no way to write this
    // loop: the worker's answer arrives on a descriptor and a request would
    // have arrived on a condition variable, and nothing could wait for both.
    const auto start = std::chrono::steady_clock::now();

    while((answered == 0 || elsewhere == 0) && seconds_since(start) < 5.0)
        r.run_one(std::chrono::milliseconds(100));

    ok("  the worker's answer arrived through the reactor", answered > 0,
       std::to_string(answered));

    ok("  and so did the unrelated descriptor", elsewhere > 0,
       std::to_string(elsewhere));

    ok("  carrying the value the worker was given",
       e.answers().size() == 1 && e.answers()[0] == 7,
       std::to_string(e.answers().size()) + " answer(s)");

    // The point.  Not "it worked" but "it worked on one thread" -- two
    // sources, one waiter, which is the thing four wakeup mechanisms could not
    // do between them.
    ok("  both dispatched on the same thread", dispatched_on.size() == 1,
       std::to_string(dispatched_on.size()) + " thread(s)");

    ok("  which is the thread that called run_one",
       dispatched_on.count(std::this_thread::get_id()) == 1);

    // The response was handled on the caller's thread and not the worker's,
    // which is the whole reason the response half is a pipe.
    ok("  and the response was handled off the worker thread",
       e.answered_on().size() == 1 &&
       e.answered_on().count(std::this_thread::get_id()) == 1);

    e.stop();
}

int main() {
    std::cout << std::unitbuf;

    a_worker_serves_what_is_pushed();
    reset_retires_the_worker_it_replaces();
    the_destructor_waits_for_the_worker();
    an_idle_worker_costs_nothing();
    one_loop_waits_on_both();
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
    // The response *reader* is pinned now -- one_loop_waits_on_both registers
    // get_response_reader() on a sys::reactor beside an unrelated descriptor
    // and shows one thread serving both, which is what that contract was
    // always for and what nothing in this tree had ever done.  Writing it
    // found that the contract did not compile: any subclass declaring
    // handle(Request) and handle(Response) hides the base's no-argument
    // handle(), which is the one an event loop is told to call, and ASMailBox
    // had no using-declaration.  A GUI holding an ASImapBox& could not make
    // the documented call.
    //
    // Not the response half's *framing*, which is still the interesting one.
    // push(Response) still writes a byte to the response pipe and still
    // *swallows* would_block on a full one -- the same fault the request side
    // shed earlier.
    // It matters more here, because that byte is what wakes an event loop
    // watching get_response_reader(): a dropped write means a response sits in
    // the queue with nothing to announce it until the next push happens to
    // succeed.  It needs 16k unread responses to reach, which is why it is
    // recorded rather than fixed, and why the request side could not simply be
    // given a blocking write instead.
    //
    // Not the idle measurement on a loaded machine.  getrusage is per-process
    // and the other threads here are asleep, so load does not add CPU to this
    // process -- but a machine thrashing hard enough to lengthen nanosleep
    // would shorten the window rather than widen it, which fails safe.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
