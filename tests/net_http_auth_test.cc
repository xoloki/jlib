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
 * server::protect(): 401 with a challenge, Basic and Bearer.
 *
 * Every section runs against both the blocking and the async server, because
 * the check sits in two dispatch functions that do not share code -- one
 * writes to a socketstream and the other co_awaits -- and a guard present in
 * one and missing from the other is exactly the shape of bug that would
 * survive a test of either alone.
 */

#include <jlib/net/http.hh>
#include <jlib/net/http_server.hh>
#include <jlib/util/URL.hh>
#include <jlib/util/util.hh>

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

static util::http::Response ask_auth(http::server& s, const std::string& path,
                                     const std::string& value)
{
    util::http::fields f;

    f.add("Authorization", value);

    try { return http::request("GET", util::URL(s.url(path)), f); }
    catch(std::exception&) { return util::http::Response(); }
}

static std::string basic(const std::string& user, const std::string& pass) {
    return "Basic " + util::base64::encode(user + ":" + pass);
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

    s.route("GET", "/admin/api/thing", [](const http::server::Request&,
                                          http::server::response& r) {
        r.status(200).type("text/plain").body("api");
    });

    s.protect("/admin/*", "Basic realm=\"jlib\"",
              [](const http::server::credentials& c) {
                  return c.user == "root" && c.password == "hunter2";
              });

    s.route("GET", "/colon/thing", [](const http::server::Request&,
                                      http::server::response& r) {
        r.status(200).type("text/plain").body("colon");
    });

    // The verifier is the assertion: it only returns true if the password
    // arrived with both its colons, so a 200 below means the split was right.
    s.protect("/colon/*", "Basic realm=\"colon\"",
              [](const http::server::credentials& c) {
                  return c.user == "root" && c.password == "a:b:c";
              });

    // More specific, and a different scheme -- the case the specificity rule
    // exists for.
    s.protect("/admin/api/*", "Bearer realm=\"api\"",
              [](const http::server::credentials& c) {
                  return c.token == "s3kr1t";
              });
}

static void what_is_open_stays_open(http::server& s) {
    std::cout << "\nwhat is not protected:\n";

    const util::http::Response r = ask(s, "/open");

    ok("  an unguarded route is untouched", r.status() == 200 &&
       r.body() == "open", std::to_string(r.status()));

    ok("  and carries no challenge", r.fields().get("WWW-Authenticate").empty(),
       r.fields().get("WWW-Authenticate"));
}

static void the_refusal(http::server& s) {
    std::cout << "\nthe 401, and what it says to do instead:\n";

    {
        const util::http::Response r = ask(s, "/admin/thing");

        ok("  no credentials is a 401", r.status() == 401,
           std::to_string(r.status()));

        ok("  with the challenge the caller registered",
           r.fields().get("WWW-Authenticate") == "Basic realm=\"jlib\"",
           r.fields().get("WWW-Authenticate"));

        // The point of the grammar work: what we emit is what a client can
        // read back.  A challenge that does not parse is a refusal that does
        // not say what to do instead.
        ok("  and that challenge parses as one",
           bool(util::http::grammar().at("WWW-Authenticate")
                .try_parse(r.fields().get("WWW-Authenticate"))));
    }

    {
        const util::http::Response r = ask_auth(s, "/admin/thing",
                                                basic("root", "wrong"));

        ok("  the wrong password is a 401", r.status() == 401,
           std::to_string(r.status()));
    }

    {
        const util::http::Response r = ask_auth(s, "/admin/thing",
                                                "Basic not-base64!!");

        // Unparseable and wrong give the same answer deliberately: saying
        // which is telling an attacker which half to keep working on.
        ok("  and so is something that is not a credential at all",
           r.status() == 401, std::to_string(r.status()));
    }

    {
        // A 404 would say the path is real, which is the one thing a 401
        // exists to withhold.
        const util::http::Response r = ask(s, "/admin/no-such-thing");

        ok("  a path that does not exist under a guard is still 401",
           r.status() == 401, std::to_string(r.status()));
    }
}

static void what_gets_through(http::server& s) {
    std::cout << "\nwhat gets through:\n";

    {
        const util::http::Response r = ask_auth(s, "/admin/thing",
                                                basic("root", "hunter2"));

        ok("  the right password is let through", r.status() == 200 &&
           r.body() == "admin", std::to_string(r.status()));
    }

    {
        // RFC 9110 11.1: auth-scheme is a token, compared case-insensitively.
        // A verifier written with == on the scheme is bypassed by lowercasing
        // it, so this is a security property and not a nicety.
        const util::http::Response r =
            ask_auth(s, "/admin/thing",
                     "bAsIc " + util::base64::encode(std::string("root:hunter2")));

        ok("  and the scheme is matched case-insensitively",
           r.status() == 200, std::to_string(r.status()));
    }

    {
        const util::http::Response r = ask_auth(s, "/admin/api/thing",
                                                "Bearer s3kr1t");

        ok("  the more specific guard wins, with its own scheme",
           r.status() == 200 && r.body() == "api", std::to_string(r.status()));
    }

    {
        // The Basic credential that opens /admin/thing must not open
        // /admin/api/thing: a guard that fell back to the looser pattern
        // would be a privilege escalation that looks like a routing detail.
        const util::http::Response r = ask_auth(s, "/admin/api/thing",
                                                basic("root", "hunter2"));

        ok("  and the looser guard's credential does not open it",
           r.status() == 401, std::to_string(r.status()));

        ok("  which is refused with the specific challenge",
           r.fields().get("WWW-Authenticate") == "Bearer realm=\"api\"",
           r.fields().get("WWW-Authenticate"));
    }
}

/**
 * RFC 7617 2: the user-id cannot contain a colon, so the *first* colon
 * separates and everything after it is the password, colons and all.
 *
 * Splitting on the last colon instead -- or on every colon -- gives a user of
 * `root` and a password of `a:b` or of `a`, and the guard's verifier says so
 * by refusing.  So a 200 here is the whole assertion.
 */
static void a_password_with_a_colon_in_it(http::server& s) {
    std::cout << "\nRFC 7617 2, the first colon separates:\n";

    {
        const util::http::Response r =
            ask_auth(s, "/colon/thing", basic("root", "a:b:c"));

        ok("  the password keeps every colon after the first",
           r.status() == 200 && r.body() == "colon",
           std::to_string(r.status()));
    }

    {
        // And the guard is really running -- otherwise the 200 above would
        // mean nothing more than that the route exists.
        const util::http::Response r =
            ask_auth(s, "/colon/thing", basic("root", "a:b"));

        ok("  and a password that stops short is refused", r.status() == 401,
           std::to_string(r.status()));
    }

    {
        // No colon at all is not a Basic credential, rather than a user with
        // an empty password.
        const util::http::Response r =
            ask_auth(s, "/colon/thing",
                     "Basic " + util::base64::encode(std::string("rootnocolon")));

        ok("  a credential with no colon is not one", r.status() == 401,
           std::to_string(r.status()));
    }

    {
        // Base64 that decodes to the right bytes but is not well formed --
        // one "=" too many.  util::base64::decode is lenient by design,
        // because RFC 2045 requires MIME to ignore what is not in the
        // alphabet, and that leniency is wrong for a credential: it accepts
        // something the client did not send.  read_credentials asks for the
        // clean flag and refuses; dropping that check lets this through.
        // A "." is legal in a token68 and is not in the base64 alphabet, so
        // the credential parses and the decode has to skip a character to
        // read it.  The bytes that come out are exactly right, which is the
        // point: leniency accepts something the client did not send.
        std::string mangled = util::base64::encode(std::string("root:a:b:c"));

        mangled.insert(4, ".");

        const util::http::Response r =
            ask_auth(s, "/colon/thing", "Basic " + mangled);

        ok("  and base64 that is not well formed is refused, not repaired",
           r.status() == 401, std::to_string(r.status()));
    }
}

static void registration_refuses_what_cannot_work(http::server& s) {
    std::cout << "\nwhat protect() refuses at registration:\n";

    struct { const char* challenge; const char* why; } bad[] = {
        { "",                  "an empty challenge, which says nothing" },
        { "Basic realm=\"x\"\r\nX-Evil: 1", "a challenge with a CRLF in it" },
        { "=not-a-scheme",     "something that is not a challenge" }
    };

    for(std::size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        bool threw = false;
        std::string why;

        try {
            s.protect("/never/*", bad[i].challenge,
                      [](const http::server::credentials&) { return true; });
        }
        catch(std::exception& e) { threw = true; why = e.what(); }

        ok(std::string("  ") + bad[i].why, threw, why);
    }

    bool threw = false;

    try { s.protect("/never/*", "Basic realm=\"x\"", http::server::verifier()); }
    catch(std::exception&) { threw = true; }

    ok("  and no verifier at all", threw);
}

static void run_them(http::server& s, const char* which) {
    std::cout << "\n-- " << which << " --\n";

    what_is_open_stays_open(s);
    the_refusal(s);
    what_gets_through(s);
    a_password_with_a_colon_in_it(s);
}

int main() {
    std::cout << "net_http_auth_test\n";

    try {
        {
            http::server s(0, "127.0.0.1");

            furnish(s);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            running go(s);

            run_them(s, "the blocking server");
        }

        {
            http::server s(http::server::async_t(), 0, "127.0.0.1");

            furnish(s);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            running go(s);

            run_them(s, "the async server");
        }

        {
            http::server s(0, "127.0.0.1");

            registration_refuses_what_cannot_work(s);
        }
    }
    catch(std::exception& e) {
        std::cerr << "net_http_auth_test: " << e.what() << "\n";
        return 1;
    }

    if(failures) {
        std::cerr << "net_http_auth_test: " << failures << " failed\n";
        return 1;
    }

    std::cout << "net_http_auth_test: all good\n";

    return 0;
}
