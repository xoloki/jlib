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

/**
 * What a program has to do to become a server: absolute paths, an identity to
 * drop to, and a fork that says whether it worked.
 *
 * These were written inside jhttpd, which was the wrong place -- `fork` and
 * `setuid` are no more HTTP-specific than `sys::run` is, and the next server
 * would have copied them. They live beside `run()`, `nosigpipe()` and
 * `sigpipe_guard` now, which is the company they keep.
 */

#include <jlib/sys/sys.hh>

#include <pthread.h>

#include <cstring>

#include <atomic>
#include <csignal>
#include <thread>
#include <chrono>
#include <iostream>
#include <string>

#include <limits.h>
#include <pwd.h>
#include <unistd.h>

using namespace jlib;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/**
 * Paths, which matter because `daemon::start()` chdirs to "/".
 *
 * A relative path resolved after that move means a different file, with no
 * symptom except the wrong one -- or a 404 for everything, which points
 * nowhere near the cause.
 */
static void paths_survive_a_chdir() {
    std::cout << "\nabsolute_path:\n";

    char here[PATH_MAX];

    if(::getcwd(here, sizeof here) == 0) {
        std::cout << "  skip  no working directory\n";

        return;
    }

    ok("  a relative path is anchored where the program started",
       sys::absolute_path("www") == std::string(here) + "/www",
       sys::absolute_path("www"));

    ok("  an absolute one is left alone",
       sys::absolute_path("/srv/www") == "/srv/www");

    // Conventional spellings, not paths.  Turning "-" into one would make a
    // program asked to log to stdout write a file called "-" instead.
    ok("  \"-\" is not a path", sys::absolute_path("-") == "-");
    ok("  and neither is empty", sys::absolute_path("").empty());
}

/**
 * Resolving an identity, and checking one.
 *
 * **The drop itself cannot run here**, and saying so is more useful than
 * implying coverage: `setgroups`, `setgid` and `setuid` need privilege that
 * `make check` does not have, and a suite that ran as root to get it would be
 * a worse idea than the gap.
 *
 * What is testable is everything around those three calls -- the lookup that
 * must happen before the drop, the failure that must stop the program, and the
 * verification that must run after. That verification is the step
 * implementations leave out, which is why it is a function of its own.
 */
static void an_identity_to_become() {
    std::cout << "\nresolving and checking an identity:\n";

    const struct passwd* me = ::getpwuid(::getuid());

    if(me == 0) {
        std::cout << "  skip  cannot look up the running user\n";

        return;
    }

    const std::string myname = me->pw_name;

    {
        const sys::identity who = sys::resolve_identity(myname, "");

        ok("  a name resolves to the ids it names",
           who.uid == ::getuid() && who.gid == ::getgid(),
           std::to_string(who.uid) + ":" + std::to_string(who.gid));
    }

    {
        bool threw = false;
        std::string why;

        try { sys::resolve_identity("nosuchuser-jlibtest", ""); }
        catch(std::exception& e) { threw = true; why = e.what(); }

        ok("  an unknown user is an error, not a silent no-op", threw, why);
    }

    {
        bool threw = false;

        try { sys::resolve_identity(myname, "nosuchgroup-jlibtest"); }
        catch(std::exception&) { threw = true; }

        ok("  and so is an unknown group", threw);
    }

    {
        // Becoming who you already are: no syscall that needs privilege, and
        // every check still runs.  That early return is not a test
        // affordance -- a process told to drop to the user it is already
        // running as has nothing to give up.
        bool threw = false;
        std::string why;

        try { sys::become(sys::resolve_identity(myname, "")); }
        catch(std::exception& e) { threw = true; why = e.what(); }

        ok("  becoming who you already are succeeds and changes nothing",
           !threw && ::getuid() == me->pw_uid, why);
    }

    {
        bool threw = false;

        try { sys::verify_identity(sys::resolve_identity(myname, "")); }
        catch(std::exception&) { threw = true; }

        ok("  the check passes for the identity this process has", !threw);
    }

    if(::geteuid() != 0) {
        // The direction that matters: a check that always passes is the same
        // as no check, and looks identical in review.
        sys::identity root;

        root.uid = 0;
        root.gid = 0;

        bool threw = false;
        std::string why;

        try { sys::verify_identity(root); }
        catch(std::exception& e) { threw = true; why = e.what(); }

        ok("  and fails for one it does not", threw, why);
    }

    std::cout << (::geteuid() == 0
                  ? "  .. running as root, so the drop itself is exercised\n"
                  : "  .. not root, so setgroups/setgid/setuid are not run\n");
}

/**
 * The daemon, which is not exercised here and should say so.
 *
 * Asserting it means forking a real background process out of a test: the
 * parent would have to wait, the child would have to be killed, and a run that
 * failed partway would leave a daemon behind. That is a process-level
 * experiment rather than a unit one.
 *
 * What *is* asserted is the part with no fork in it -- that a `daemon` nobody
 * started is safe to carry and to signal, which is what lets a program hold one
 * unconditionally and only daemonise when asked.
 */
static void a_daemon_that_was_never_started() {
    std::cout << "\na daemon nobody asked for:\n";

    sys::daemon never;

    never.ready();
    never.ready();

    ok("  ready() on one that never started does nothing, twice", true);
}


/**
 * The self-pipe a signal handler writes to.
 *
 * The thing under test is the contract the handler depends on: that a signal
 * arriving while a thread is blocked in wait() wakes it, that the count says
 * which signal it was, and that a wait with nothing pending actually blocks
 * rather than spinning.
 */
static void a_signal_reaches_a_thread() {
    std::cout << "\nwaking a thread from a handler:\n";

    ok("  arming opens the pipe", sys::wakeup::arm());
    ok("  and arming twice is not an error", sys::wakeup::arm());

    std::signal(SIGUSR1, sys::wakeup::on_signal);
    std::signal(SIGUSR2, sys::wakeup::on_signal);

    const std::sig_atomic_t before = sys::wakeup::count(SIGUSR1);

    // Raised from this thread, which is the hard case rather than the easy
    // one: the handler runs on the thread that will wait, so a byte that was
    // not actually written would hang the wait below forever.
    ::raise(SIGUSR1);

    ok("  the signal was counted",
       sys::wakeup::count(SIGUSR1) == before + 1,
       std::to_string(sys::wakeup::count(SIGUSR1)));

    ok("  and a waiter does not block on it, calling it a signal",
       sys::wakeup::wait() == sys::wakeup::woken::signalled);

    // Counts are per signal, not one total.
    const std::sig_atomic_t one = sys::wakeup::count(SIGUSR1);
    const std::sig_atomic_t two = sys::wakeup::count(SIGUSR2);

    ::raise(SIGUSR2);

    ok("  a different signal has its own count",
       sys::wakeup::count(SIGUSR2) == two + 1 &&
           sys::wakeup::count(SIGUSR1) == one);

    ok("  which also wakes a waiter",
       sys::wakeup::wait() == sys::wakeup::woken::signalled);

    // **It really blocks.**  Without this the section above passes on an
    // implementation whose wait() returns immediately every time, which would
    // make every caller a busy loop.
    {
        // **The return value, not just "it came back".**  A waiter that
        // ignored it could not tell being woken from giving up, and a wait()
        // that returned false on the first hiccup would pass either way.
        std::atomic<int> got{-1};
        std::thread waiter([&got] {
            got.store(static_cast<int>(sys::wakeup::wait()));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        const bool still_waiting = got.load() == -1;

        sys::wakeup::poke();
        waiter.join();

        ok("  a wait with nothing pending blocks", still_waiting);
        ok("  and poke() wakes it, calling it a poke",
           got.load() == static_cast<int>(sys::wakeup::woken::poked),
           std::to_string(got.load()));
    }

    // A signal arriving while a thread is parked in wait() is the case the
    // whole thing exists for.
    {
        std::atomic<int> got{-1};
        std::thread waiter([&got] {
            got.store(static_cast<int>(sys::wakeup::wait()));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        ::raise(SIGUSR1);

        for(int i = 0; i < 100 && got.load() == -1; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        waiter.join();

        ok("  a signal wakes a thread already parked in wait()",
           got.load() == static_cast<int>(sys::wakeup::woken::signalled),
           std::to_string(got.load()));
    }

    // **Delivered to the waiting thread, which is what exercises EINTR.**
    //
    // raise() above runs the handler on *this* thread, so the waiter's read()
    // is never interrupted -- it simply finds the byte. pthread_kill aims the
    // signal at the thread that is blocked, so its read() returns EINTR and
    // the retry inside wait() is the only thing that finds the byte the
    // handler just wrote. Without that retry this answers false.
    {
        std::atomic<int> got{-1};
        std::atomic<bool> parked{false};
        pthread_t who = 0;

        std::thread waiter([&got, &parked, &who] {
            who = ::pthread_self();
            parked.store(true);
            got.store(static_cast<int>(sys::wakeup::wait()));
        });

        for(int i = 0; i < 100 && !parked.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // **Installed without SA_RESTART for this one case.**  std::signal()
        // gives BSD semantics -- the kernel restarts an interrupted read()
        // itself -- so with it EINTR never reaches wait() and the retry inside
        // is unreachable. Asking for the other behaviour is the only way to
        // put the retry under test, and it is what any caller using sigaction
        // directly will get.
        struct sigaction raw;

        std::memset(&raw, 0, sizeof raw);

        raw.sa_handler = sys::wakeup::on_signal;
        sigemptyset(&raw.sa_mask);
        raw.sa_flags = 0;

        ::sigaction(SIGUSR1, &raw, 0);

        ::pthread_kill(who, SIGUSR1);

        for(int i = 0; i < 200 && got.load() == -1; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        waiter.join();

        ok("  a signal delivered to the waiter survives EINTR",
           got.load() == static_cast<int>(sys::wakeup::woken::signalled),
           std::to_string(got.load()));

        std::signal(SIGUSR1, sys::wakeup::on_signal);
    }

    ok("  an out-of-range signal counts nothing", sys::wakeup::count(-1) == 0);
    ok("  and so does one past the end", sys::wakeup::count(NSIG) == 0);

    // Closing ends any wait rather than leaving a thread parked forever.
    {
        std::atomic<int> got{-1};
        std::thread waiter([&got] {
            got.store(static_cast<int>(sys::wakeup::wait()));
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        sys::wakeup::disarm();
        waiter.join();

        // False here is the right answer, not a failure: the pipe is gone and
        // there is nothing more to wait for.
        ok("  disarm() ends a wait, calling it closed",
           got.load() == static_cast<int>(sys::wakeup::woken::closed),
           std::to_string(got.load()));
    }

    ok("  and a wait on a closed pipe answers closed at once",
       sys::wakeup::wait() == sys::wakeup::woken::closed);

    // **A signal and a poke in one read, which is the case that decides how
    // wait() must look at the drain.**
    //
    // One read takes whatever has accumulated, so these come back together as
    // "wp". A wait() that judged by the first byte, or by the last, would be
    // a coin flip -- and losing it means answering `poked` with a signal in
    // hand, so the caller returns without acting and Ctrl-C does nothing at
    // all. Worse than the hang the enum exists to prevent, and quieter.
    ok("  re-arming after disarm", sys::wakeup::arm());

    {
        const std::sig_atomic_t before = sys::wakeup::count(SIGUSR1);

        ::raise(SIGUSR1);        // writes 'w'
        sys::wakeup::poke();     // writes 'p', before anything reads

        ok("  both landed before the read",
           sys::wakeup::count(SIGUSR1) == before + 1);

        ok("  a signal sharing a read with a poke still says signalled",
           sys::wakeup::wait() == sys::wakeup::woken::signalled);
    }

    {
        // And the other order, since the bytes would be "pw" this time.
        const std::sig_atomic_t before = sys::wakeup::count(SIGUSR1);

        sys::wakeup::poke();
        ::raise(SIGUSR1);

        ok("  and so does one that arrives after the poke",
           sys::wakeup::count(SIGUSR1) == before + 1 &&
               sys::wakeup::wait() == sys::wakeup::woken::signalled);
    }

    std::signal(SIGUSR1, SIG_DFL);
    std::signal(SIGUSR2, SIG_DFL);
}

int main() {
    a_signal_reaches_a_thread();
    std::cout << "sys_process_test\n";

    try {
        paths_survive_a_chdir();
        an_identity_to_become();
        a_daemon_that_was_never_started();
    }
    catch(std::exception& e) {
        std::cerr << "sys_process_test: " << e.what() << "\n";
        return 1;
    }

    if(failures) {
        std::cerr << "sys_process_test: " << failures << " failed\n";
        return 1;
    }

    std::cout << "sys_process_test: all good\n";

    return 0;
}
