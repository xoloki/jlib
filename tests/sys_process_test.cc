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

int main() {
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
