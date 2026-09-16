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
 * Rate limiting: the bucket, and the 429 it produces.
 *
 * Two halves, because they can be tested to different standards.
 *
 * The **algorithm** is tested directly, with addresses supplied as strings.
 * Per-address separation is the whole point of the feature and it cannot be
 * reached through the server at all: every client here connects over loopback,
 * and both platforms report the peer as 127.0.0.1 whichever loopback address
 * is dialled -- measured on macOS, where 127.0.0.2 does not route, and in the
 * Linux container, where it routes but the kernel still picks 127.0.0.1 as the
 * source.  A test that drove it through HTTP would be asserting that one
 * address is limited, twice.
 *
 * The **wiring** is tested through the server: that the check runs, that it
 * runs before protect(), and that what comes back is a 429 with a Retry-After.
 */

#include <jlib/net/http.hh>
#include <jlib/net/http_server.hh>
#include <jlib/util/URL.hh>
#include <jlib/util/util.hh>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace jlib;
using namespace jlib::net;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

struct running {
    http::server& s;
    std::thread th;

    explicit running(http::server& server)
        : s(server), th([&server]{ server.run(); }) {}

    ~running() {
        s.stop();

        if(th.joinable()) th.join();
    }

    running(const running&) = delete;
    running& operator=(const running&) = delete;
};

static util::http::Response ask(http::server& s, const std::string& path) {
    try { return http::request("GET", util::URL(s.url(path))); }
    catch(std::exception&) { return util::http::Response(); }
}

static void the_bucket() {
    std::cout << "\nthe bucket itself:\n";

    {
        http::server::limiter l;

        // Slow enough that nothing refills measurably during the test, so
        // every assertion below is about the bucket and not about the clock.
        l.configure(0.001, 3);

        long wait = 0;
        int through = 0;

        for(int i = 0; i < 10; i++) if(l.allow("10.0.0.1", wait)) through++;

        ok("  a burst of three is three, and then no more", through == 3,
           std::to_string(through));

        ok("  and the refusal says how long to wait", wait >= 1,
           std::to_string(wait));
    }

    {
        http::server::limiter l;

        l.configure(0.001, 2);

        long wait = 0;

        while(l.allow("10.0.0.1", wait)) { }

        // The whole feature: one address running out does not spend another
        // address's tokens.
        ok("  one address running out leaves another untouched",
           l.allow("10.0.0.2", wait));

        ok("  and they are counted apart", l.tracked() == 2,
           std::to_string(l.tracked()));
    }

    {
        http::server::limiter l;

        // 100 a second: a token is worth 10ms, so waiting 150ms is well past
        // one and the assertion is one-sided -- it does not care how long it
        // actually took, only that waiting longer than the refill works.
        l.configure(100, 1);

        long wait = 0;

        ok("  the first request of a burst of one is allowed",
           l.allow("10.0.0.3", wait));

        ok("  the second, immediately, is not", !l.allow("10.0.0.3", wait));

        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        ok("  and after longer than it takes to refill, it is again",
           l.allow("10.0.0.3", wait));
    }

    {
        http::server::limiter l;

        // A burst below one token cannot hold a whole one, so every request
        // would be refused -- a limit of "none at all", spelled as if it were
        // a number.  Clamped rather than thrown on, because zero is what a
        // caller writes when they mean "no burst allowance", not "no service".
        l.configure(1000, 0);

        long wait = 0;

        ok("  a burst of zero is clamped to one rather than refusing all",
           l.allow("10.0.0.5", wait));
    }

    {
        http::server::limiter l;

        // Off is off, and remembers nobody -- so a server that never calls
        // rate_limit() pays nothing, not even the table.
        l.configure(0, 5);

        long wait = 0;
        bool all = true;

        for(int i = 0; i < 100; i++) if(!l.allow("10.0.0.4", wait)) all = false;

        ok("  a rate of zero allows everything", all);
        ok("  and remembers nobody", l.tracked() == 0,
           std::to_string(l.tracked()));
    }
}

static void the_table_does_not_grow_without_bound() {
    std::cout << "\nthe sweep:\n";

    http::server::limiter l;

    // Fast, so a bucket refills to full in the time it takes to reach the
    // next address -- which is exactly when dropping it is free.
    l.configure(100000, 4);

    long wait = 0;

    for(int i = 0; i < 20000; i++)
        l.allow("10.1." + std::to_string(i / 250) + "." + std::to_string(i % 250),
                wait);

    ok("  twenty thousand addresses do not become twenty thousand buckets",
       l.tracked() < 20000, std::to_string(l.tracked()) + " kept");

    // And the sweep is exact rather than lossy: a swept address is in the same
    // state as one never seen, because both start full.
    int through = 0;

    for(int i = 0; i < 4; i++) if(l.allow("10.1.0.0", wait)) through++;

    ok("  and a swept address still gets its whole burst", through == 4,
       std::to_string(through));

    // The other direction, which is the one that matters: a sweep must not
    // *grant* anything.  An address that has spent its tokens and cannot
    // refill is not full, so an exact sweep keeps it -- while a sweep that
    // dropped whatever it liked would hand a client a fresh burst for the
    // price of making the table grow, which is a limit an attacker can reset.
    http::server::limiter k;

    k.configure(0.001, 2);

    while(k.allow("10.9.9.9", wait)) { }

    for(int i = 0; i < 20000; i++)
        k.allow("10.2." + std::to_string(i / 250) + "." + std::to_string(i % 250),
                wait);

    ok("  and a sweep never hands back a bucket that was spent",
       !k.allow("10.9.9.9", wait));

    // **The bound, which the exact pass alone does not give.**  At this rate
    // nothing ever refills, so no bucket is ever full and the exact pass frees
    // nothing at all -- measured, not predicted: before the hard bound existed,
    // 50,000 addresses kept 50,000 buckets and every request past the trigger
    // walked all of them.  An attacker with a v6 allocation has as many source
    // addresses as it wants, so an unbounded table turns a defence against
    // flooding into a way to amplify one.
    ok("  and the table is bounded even when nothing can be swept exactly",
       k.tracked() <= 4096, std::to_string(k.tracked()) + " kept");
}

static void through_the_server(http::server& s) {
    std::cout << "\nthrough the server:\n";

    // Two a second, one at a time, so the second request is refused and the
    // wait is short enough to be worth asserting nothing about.
    s.rate_limit(0.001, 1);

    {
        const util::http::Response r = ask(s, "/open");

        ok("  the first request is answered", r.status() == 200,
           std::to_string(r.status()));
    }

    {
        const util::http::Response r = ask(s, "/open");

        ok("  the second is a 429", r.status() == 429,
           std::to_string(r.status()));

        ok("  carrying a Retry-After in seconds",
           !r.fields().get("Retry-After").empty() &&
           std::atol(r.fields().get("Retry-After").c_str()) >= 1,
           r.fields().get("Retry-After"));
    }

    {
        // Before protect(), not after: a 429 has to be reachable without
        // credentials, or the rate limit protects everything except the thing
        // most worth protecting.
        const util::http::Response r = ask(s, "/admin/thing");

        ok("  a protected path is rate-limited before it is authenticated",
           r.status() == 429, std::to_string(r.status()));
    }

    // Off again, so the assertion below is about the switch and not about
    // whatever the bucket happens to hold.
    s.rate_limit(0, 1);

    {
        const util::http::Response r = ask(s, "/open");

        ok("  and turning it off lets everything through again",
           r.status() == 200, std::to_string(r.status()));
    }

    {
        // Still authenticated: turning the limiter off must not have turned
        // anything else off with it.
        const util::http::Response r = ask(s, "/admin/thing");

        ok("  while protect() still refuses without credentials",
           r.status() == 401, std::to_string(r.status()));
    }
}

static void furnish(http::server& s) {
    s.route("GET", "/open", [](const http::server::Request&,
                               http::server::response& r) {
        r.status(200).type("text/plain").body("open");
    });

    s.route("GET", "/admin/thing", [](const http::server::Request&,
                                      http::server::response& r) {
        r.status(200).type("text/plain").body("admin");
    });

    s.protect("/admin/*", "Basic realm=\"jlib\"",
              [](const http::server::credentials& c) {
                  return c.user == "root" && c.password == "hunter2";
              });
}

int main() {
    std::cout << "net_http_ratelimit_test\n";

    try {
        the_bucket();
        the_table_does_not_grow_without_bound();

        {
            http::server s(0, "127.0.0.1");

            furnish(s);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            running go(s);

            std::cout << "\n-- the blocking server --";
            through_the_server(s);
        }

        {
            http::server s(http::server::async_t(), 0, "127.0.0.1");

            furnish(s);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            running go(s);

            std::cout << "\n-- the async server --";
            through_the_server(s);
        }
    }
    catch(std::exception& e) {
        std::cerr << "net_http_ratelimit_test: " << e.what() << "\n";
        return 1;
    }

    if(failures) {
        std::cerr << "net_http_ratelimit_test: " << failures << " failed\n";
        return 1;
    }

    std::cout << "net_http_ratelimit_test: all good\n";

    return 0;
}
