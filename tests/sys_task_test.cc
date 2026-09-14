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
 * sys_task_test -- the coroutine primitives.
 *
 * Colours nothing.  These are the pieces every async framing function will be
 * built from, tested against pipes and socketpairs with no protocol code in
 * the way.
 *
 * Determinism follows the reactor test's two rules: cause the readiness in
 * this thread before the wait, and assert on what happened rather than on how
 * long it took.
 */

#include "feed.hh"

#include <jlib/sys/await.hh>
#include <jlib/sys/pipe.hh>
#include <jlib/sys/reactor.hh>
#include <jlib/sys/task.hh>

#include <unistd.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
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

/** A task does not run until it is awaited or started. */
static sys::task<int> counts(int& ran) {
    ran++;

    co_return 7;
}

static void a_task_is_lazy() {
    std::cout << "\na task is lazy:\n";

    int ran = 0;

    sys::task<int> t = counts(ran);

    // Eager would have run it on the thread that constructed it, which for an
    // I/O task is the wrong thread by definition.
    ok("  constructing it runs nothing", ran == 0, std::to_string(ran));

    t.start();

    ok("  starting it runs the body", ran == 1, std::to_string(ran));

    ok("  and it is finished", t.done());

    ok("  carrying its value", t.result() == 7);
}

static sys::task<int> inner() { co_return 6; }

static sys::task<int> outer() {
    const int n = co_await inner();

    co_return n * 7;
}

static void tasks_compose() {
    std::cout << "\ntasks compose:\n";

    sys::task<int> t = outer();

    t.start();

    ok("  awaiting one from another works", t.done() && t.result() == 42,
       std::to_string(t.result()));
}

static sys::task<void> throws_after(int depth) {
    if(depth == 0) throw std::runtime_error("from the bottom");

    co_await throws_after(depth - 1);
}

static void an_exception_comes_back_out() {
    std::cout << "\nan exception comes back out:\n";

    {
        sys::task<void> t = throws_after(0);

        t.start();

        bool threw = false;
        std::string why;

        try { t.result(); }
        catch(std::exception& e) { threw = true; why = e.what(); }

        ok("  from the task itself", threw && why == "from the bottom", why);
    }

    {
        // Through five awaits, each of which has to rethrow rather than
        // swallow.
        sys::task<void> t = throws_after(5);

        t.start();

        bool threw = false;

        try { t.result(); }
        catch(std::runtime_error&) { threw = true; }

        ok("  and up through a chain of awaits", threw);
    }
}

/**
 * The one whose failure mode is a crash rather than a wrong answer.
 *
 * final_suspend returns the waiter's handle instead of resuming it, which is
 * symmetric transfer and compiles to a tail call.  Resuming directly would
 * grow the *real* stack by a frame per await, so this chain would overflow it
 * -- at some depth nobody has measured, under load, in production.  The whole
 * point of the test is that there is no such depth.
 */
static sys::task<int> deep(int n) {
    if(n == 0) co_return 0;

    co_return 1 + co_await deep(n - 1);
}

static void a_deep_chain_does_not_grow_the_stack() {
    std::cout << "\na deep chain does not grow the stack:\n";

    const int DEPTH = 20000;

    sys::task<int> t = deep(DEPTH);

    t.start();

    ok("  twenty thousand awaits complete", t.done() && t.result() == DEPTH,
       std::to_string(t.result()));
}

/** Suspend on a descriptor, resume when it is readable. */
static sys::task<std::string> read_one(sys::reactor& r, int fd,
                                       sys::cancel_token t)
{
    co_await sys::readable(r, fd, t);

    char buf[64];
    const ssize_t n = ::read(fd, buf, sizeof buf);

    co_return n > 0 ? std::string(buf, std::size_t(n)) : std::string();
}

static void a_task_suspends_on_a_descriptor() {
    std::cout << "\na task suspends on a descriptor:\n";

    sys::reactor r;
    sys::pipe p(false, false);

    sys::cancel_token none;

    sys::task<std::string> t = read_one(r, p.get_reader(), none);

    t.start();

    ok("  it suspends rather than finishing", !t.done());

    ok("  and registered with the reactor", r.count() == 1,
       std::to_string(r.count()));

    const char* msg = "hello";

    feed(p.get_writer(), msg, 5);

    const std::string got = sys::run_until_complete(r, t);

    ok("  then resumes with what arrived", got == "hello", "\"" + got + "\"");

    ok("  and the registration is gone", r.count() == 0,
       std::to_string(r.count()));
}

/**
 * Destroying a task while it is suspended.
 *
 * The reactor holds a registration whose callback captured the coroutine
 * handle.  If the frame is destroyed first, that handle dangles and the next
 * readiness resumes freed memory -- which ASan reports as a
 * heap-use-after-free *inside the coroutine*, and which this is written to
 * prevent.
 *
 * The fix is that the awaiter is a temporary the coroutine keeps alive across
 * the suspension, so it lives in the frame and its destructor runs when the
 * frame does; it unregisters there.  Verified by removing that destructor and
 * watching ASan report the use-after-free.
 */
static void a_task_destroyed_while_parked_unregisters() {
    std::cout << "\na task destroyed while parked unregisters:\n";

    sys::reactor r;
    sys::pipe p(false, false);
    sys::cancel_token none;

    {
        sys::task<std::string> t = read_one(r, p.get_reader(), none);

        t.start();

        ok("  it is parked", !t.done() && r.count() == 1,
           std::to_string(r.count()));
    }

    ok("  and destroying it takes the registration with it", r.count() == 0,
       std::to_string(r.count()));

    // Would resume a freed frame if the registration had survived.  Without
    // a sanitizer this may pass either way, which the closing note says.
    feed(p.get_writer(), "x", 1);

    r.run_one(std::chrono::milliseconds(100));

    ok("  so making the descriptor ready afterwards is harmless", true);
}

static void cancellation() {
    std::cout << "\ncancellation:\n";

    {
        sys::reactor r;
        sys::pipe p(false, false);
        sys::cancel_token t = sys::cancel_token::create();

        sys::task<std::string> task = read_one(r, p.get_reader(), t);

        task.start();

        ok("  a task parked on a descriptor is not done", !task.done());

        // **Nothing is ever written to this pipe.**  An earlier version had to
        // write a byte here to wake the pass, because request() only set a
        // flag and left the wait registered -- so the await did not end until
        // the descriptor happened to become ready, which for a peer that never
        // speaks is never.  That is the slow-loris shape, and the write was
        // the workaround for it.
        //
        // request() ends the wait now, so the workaround is gone and its
        // absence is the assertion.
        t.request();

        ok("  cancelling resumes it there and then", task.done());

        ok("  and the reactor has nothing left registered", r.count() == 0,
           std::to_string(r.count()));

        bool threw = false;

        try { task.result(); }
        catch(sys::cancelled&) { threw = true; }

        ok("  with the await throwing cancelled", threw);
    }

    {
        sys::reactor r;
        sys::pipe p(false, false);
        sys::cancel_token t = sys::cancel_token::create();

        // Set *before* the task starts.
        t.request();

        sys::task<std::string> task = read_one(r, p.get_reader(), t);

        task.start();

        ok("  a token already set does not suspend at all", task.done());

        ok("  and nothing was registered", r.count() == 0,
           std::to_string(r.count()));

        bool threw = false;

        try { task.result(); }
        catch(sys::cancelled&) { threw = true; }

        ok("  it throws without waiting", threw);
    }

    {
        sys::cancel_token a = sys::cancel_token::create();
        sys::cancel_token b = a;

        a.request();

        // Shared, not copied: cancelling an outer operation has to cancel
        // everything nested below it, which is what a connection teardown
        // means.
        ok("  a copied token shares the flag", b.requested());
    }
}

/**
 * A deadline against a peer that never speaks.
 *
 * The shape #166 asks for -- "Pop3 and Imap4 wait forever by default, and a
 * wedged server hangs the caller" -- and the one a slow-loris defence takes:
 * not a timeout threaded through every call, but a timer that cancels what is
 * in flight.
 *
 * Nothing is written to the pipe at any point.  The only thing that ends the
 * read is the deadline firing.
 */
/** A default-constructed token cannot fire, and says so rather than lying. */
static void a_default_token_refuses_to_pretend() {
    std::cout << "\na default token refuses to pretend:\n";

    sys::cancel_token none;

    ok("  it is not live", !none.live());

    ok("  and never reads as requested", !none.requested());

    bool threw = false;

    // The alternative was a silent no-op, which is how a caller writes a
    // cancellation that does nothing and finds out much later.
    try { none.request(); }
    catch(sys::cancel_token::misuse&) { threw = true; }

    ok("  and requesting it is an error, not a no-op", threw);
}

/**
 * One token, several waits.
 *
 * What a connection teardown means: cancelling the outer operation has to end
 * everything nested below it, and a token is shared rather than copied so that
 * it does.
 */
static void one_cancel_ends_every_wait_on_the_token() {
    std::cout << "\none cancel ends every wait on the token:\n";

    sys::reactor r;
    sys::pipe a(false, false);
    sys::pipe b(false, false);
    sys::pipe c(false, false);

    sys::cancel_token t = sys::cancel_token::create();

    // A copy, as a nested operation would be handed.
    sys::cancel_token nested = t;

    sys::task<std::string> one = read_one(r, a.get_reader(), t);
    sys::task<std::string> two = read_one(r, b.get_reader(), nested);
    sys::task<std::string> three = read_one(r, c.get_reader(), nested);

    one.start();
    two.start();
    three.start();

    ok("  three reads are parked", r.count() == 3, std::to_string(r.count()));

    t.request();

    ok("  one request ends all of them",
       one.done() && two.done() && three.done());

    ok("  and the reactor is empty", r.count() == 0,
       std::to_string(r.count()));

    int threw = 0;

    for(sys::task<std::string>* p : { &one, &two, &three }) {
        try { p->result(); }
        catch(sys::cancelled&) { threw++; }
    }

    ok("  each throwing cancelled", threw == 3, std::to_string(threw));
}

static void a_deadline_ends_a_wait_that_would_not() {
    std::cout << "\na deadline ends a wait that would not:\n";

    sys::reactor r;
    sys::pipe p(false, false);

    sys::cancel_token t = sys::cancel_token::create();

    sys::task<std::string> task = read_one(r, p.get_reader(), t);

    task.start();

    ok("  the read is parked on a silent peer", !task.done());

    sys::deadline(r, std::chrono::milliseconds(80), t);

    const auto start = std::chrono::steady_clock::now();

    bool threw = false;

    try { sys::run_until_complete(r, task); }
    catch(sys::cancelled&) { threw = true; }

    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
        .count();

    ok("  the deadline ends it", threw);

    // One-sided below, generous above: early would be a bug, late is a loaded
    // machine.  Before this commit it would not have ended at all.
    ok("  at about the time it was set for", took >= 0.07 && took < 3.0,
       std::to_string(took) + "s");

    ok("  and nothing is left registered", r.count() == 0,
       std::to_string(r.count()));
}

/** A deadline that does not fire, because the work finished first. */
static void a_deadline_that_is_not_needed() {
    std::cout << "\na deadline that is not needed:\n";

    sys::reactor r;
    sys::pipe p(false, false);

    sys::cancel_token t = sys::cancel_token::create();

    sys::task<std::string> task = read_one(r, p.get_reader(), t);

    task.start();

    const sys::reactor::timer_token timer =
        sys::deadline(r, std::chrono::seconds(30), t);

    feed(p.get_writer(), "quick", 5);

    const std::string got = sys::run_until_complete(r, task);

    ok("  the read completes normally", got == "quick", got);

    // Cancelled, or a thirty-second timer sits in the reactor holding the
    // token -- and would fire into whatever uses it next.
    r.cancel(timer);

    ok("  and the deadline can be taken back off", r.timers() == 0,
       std::to_string(r.timers()));
}

static void sleeping() {
    std::cout << "\nsleeping:\n";

    sys::reactor r;
    sys::cancel_token none;

    const auto start = std::chrono::steady_clock::now();

    sys::task<void> t = [](sys::reactor& r, sys::cancel_token c) -> sys::task<void> {
        co_await sys::sleep_for(r, std::chrono::milliseconds(50), c);
    }(r, none);

    sys::run_until_complete(r, t);

    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
        .count();

    // One-sided, as the reactor's timer tests are: late is a loaded machine,
    // early is a bug.
    ok("  a sleep is never short", took >= 0.05, std::to_string(took) + "s");
}

/** run_until_complete is what keeps a synchronous caller synchronous. */
static void a_synchronous_caller_stays_synchronous() {
    std::cout << "\na synchronous caller stays synchronous:\n";

    sys::reactor r;
    sys::pipe p(false, false);
    sys::cancel_token none;

    feed(p.get_writer(), "already here", 12);

    // This function is not a coroutine and does not become one.  That is the
    // whole property: Imap4 and jlib-mail can keep their blocking shape.
    sys::task<std::string> t = read_one(r, p.get_reader(), none);

    const std::string got = sys::run_until_complete(r, t);

    ok("  it gets the value without being a coroutine itself",
       got == "already here", "\"" + got + "\"");
}

int main() {
    std::cout << std::unitbuf;

    a_task_is_lazy();
    tasks_compose();
    an_exception_comes_back_out();
    a_deep_chain_does_not_grow_the_stack();
    a_task_suspends_on_a_descriptor();
    a_task_destroyed_while_parked_unregisters();
    cancellation();
    a_default_token_refuses_to_pretend();
    one_cancel_ends_every_wait_on_the_token();
    a_deadline_ends_a_wait_that_would_not();
    a_deadline_that_is_not_needed();
    sleeping();
    a_synchronous_caller_stays_synchronous();

    // What a green run does not establish.
    //
    // That anything in jlib uses these.  Nothing does: no protocol code is
    // coloured and no signature changed, which was the point of doing the
    // primitives first.  The framing functions come next, one at a time.
    //
    // That cancellation *unregisters* anything.  Destroying a parked task
    // does -- that is the section above -- but a token merely *requested*
    // leaves the await registered until the descriptor happens to become
    // ready, so a cancelled read on a socket that never speaks holds an entry
    // until the connection closes.  Enough for a token to mean "throw when you
    // next wake"; not enough for a slow-loris defence, which needs the wait to
    // end on the token rather than on the descriptor.
    //
    // The use-after-free that section guards against is only reliably caught
    // under ASan.  Without it the test may pass on a build that has the bug.
    //
    // Nothing about migration between threads.  Every test here resumes on the
    // thread that started the task, because the reactor is single threaded.  A
    // pool hop -- co_await on_pool{...} -- is not written yet and the hazards
    // it brings are recorded in task.hh rather than tested.
    //
    // And the deep-chain test measures twenty thousand, not "any depth".  It
    // would catch a lost symmetric transfer; it does not prove there is no
    // depth at all.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
