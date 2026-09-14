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
 * net_imap_async_test -- imap::read, suspending.
 *
 * The same question as util_http_async_test asks of read_head and read_body:
 * **do the two agree**, including about what they refuse.  A literal is the
 * one thing in IMAP that cannot be framed by looking for a line ending, so a
 * suspending version that got it subtly wrong would read a message body as
 * protocol and say nothing about it.
 */

#include <jlib/net/imap_response.hh>
#include <jlib/sys/async_reader.hh>
#include <jlib/sys/await.hh>
#include <jlib/sys/reactor.hh>

#include <unistd.h>

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace imap = jlib::net::imap;
namespace sys = jlib::sys;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static std::string show(const std::string& s) {
    std::string out;

    for(std::size_t i = 0; i < s.size(); i++) {
        if(s[i] == '\r') out += "\\r";
        else if(s[i] == '\n') out += "\\n";
        else out += s[i];
    }

    return out;
}

struct outcome {
    bool threw = false;
    std::string value;
    std::string why;

    bool operator==(const outcome& o) const {
        return threw == o.threw && value == o.value && why == o.why;
    }
};

static outcome sync_read(const std::string& in) {
    outcome o;
    std::istringstream is(in);

    try { o.value = imap::read(is); }
    catch(std::exception& e) { o.threw = true; o.why = e.what(); }

    return o;
}

static outcome async_read(const std::string& in, std::size_t chunk) {
    outcome o;

    sys::reactor r;

    int fds[2];

    if(::pipe(fds) != 0) { o.threw = true; o.why = "pipe() failed"; return o; }

    std::thread feeder([&]{
        for(std::size_t at = 0; at < in.size(); at += chunk) {
            const std::string part = in.substr(at, chunk);

            std::this_thread::sleep_for(std::chrono::milliseconds(1));

            ::write(fds[1], part.data(), part.size());
        }

        ::close(fds[1]);
    });

    sys::async_reader in_r(r, fds[0]);
    sys::task<std::string> t = imap::read(in_r);

    try { o.value = sys::run_until_complete(r, t); }
    catch(std::exception& e) { o.threw = true; o.why = e.what(); }

    feeder.join();

    ::close(fds[0]);

    return o;
}

static void they_agree(const std::string& what, const std::string& input) {
    const outcome a = sync_read(input);

    // One octet at a time is the worst case: a suspension inside the line,
    // inside the literal introducer, and inside the literal's octets.
    const outcome b = async_read(input, 1);

    const outcome c = async_read(input, input.empty() ? 1 : input.size());

    ok("  " + what, a == b && a == c,
       a == b ? (a == c ? "" : "differs when delivered whole")
              : "differs when delivered one octet at a time");

    if(!(a == b)) {
        std::cout << "         sync : "
                  << (a.threw ? "threw " + a.why : "\"" + show(a.value) + "\"")
                  << "\n";
        std::cout << "         async: "
                  << (b.threw ? "threw " + b.why : "\"" + show(b.value) + "\"")
                  << "\n";
    }
}

static void the_two_reads_agree() {
    std::cout << "\nthe synchronous and suspending imap::read agree:\n";

    they_agree("a plain tagged response", "a001 OK done\r\n");

    they_agree("an untagged one", "* 3 EXISTS\r\n");

    they_agree("a literal", "* 1 FETCH (BODY[] {5}\r\nhello)\r\n");

    they_agree("a literal containing CRLF",
               "* 1 FETCH (BODY[] {7}\r\na\r\nb\r\n)\r\n");

    // The case the header means by "a response is not a line": the literal's
    // octets look exactly like protocol and must not be read as any.
    they_agree("a literal containing what looks like a response",
               "* 1 FETCH (BODY[] {16}\r\n* 2 EXISTS\r\nxxx\r\n)\r\n");

    they_agree("a literal ending in CR",
               "* 1 FETCH (BODY[] {3}\r\na\r\r\n)\r\n");

    they_agree("an empty literal", "* 1 FETCH (BODY[] {0}\r\n)\r\n");

    they_agree("two literals on one line",
               "* 1 FETCH (A {2}\r\nab B {3}\r\ncde)\r\n");

    they_agree("a non-synchronising literal", "* 1 FETCH (BODY[] {5+}\r\nhello)\r\n");

    // The refusals, and the partial-line case where getline's rule is odd.
    they_agree("nothing at all", "");

    they_agree("a line with no ending", "a001 OK done");

    they_agree("a literal that ends early", "* 1 FETCH (BODY[] {50}\r\nshort");

    they_agree("a bare LF line ending", "a001 OK done\n");

    they_agree("a literal introduced then nothing", "* 1 FETCH (BODY[] {5}\r\n");
}

/** The parser is untouched: it takes a string_view and is not a coroutine. */
static void the_parser_is_untouched() {
    std::cout << "\nthe parser is untouched:\n";

    sys::reactor r;

    int fds[2];

    if(::pipe(fds) != 0) { ok("  a pipe", false); return; }

    const std::string wire = "* 1 FETCH (BODY[] {5}\r\nhello)\r\n";

    ::write(fds[1], wire.data(), wire.size());
    ::close(fds[1]);

    sys::async_reader in(r, fds[0]);
    sys::task<std::string> t = imap::read(in);

    const std::string raw = sys::run_until_complete(r, t);

    ok("  the coroutine returns the whole response", raw == wire,
       show(raw));

    const imap::response parsed = imap::response::parse(raw);

    ok("  and it parses as untagged data",
       parsed.type() == imap::response::kind::untagged);

    ::close(fds[0]);
}

int main() {
    std::cout << std::unitbuf;

    the_two_reads_agree();
    the_parser_is_untouched();

    // What a green run does not establish.
    //
    // That anything awaits it.  Imap4 and ASImapBox both still call the
    // blocking imap::read, and both remain.
    //
    // That a real server sends any of these.  Every input here was written by
    // hand; net_imap_live_test is where jlib meets a real Dovecot, and it
    // exercises the *blocking* path only.
    //
    // Nothing about TLS, which is how a real IMAP connection is carried.
    // async_reader reads a descriptor, and a TLS stream answers a second
    // readiness question that a descriptor cannot -- #4 records that as the
    // real work behind the issue.
    //
    // And the partial-line rule this matches is odd rather than right:
    // std::getline succeeds on a line truncated at end of stream and fails
    // only on the next call, so a response cut mid-line comes back as though
    // it were whole.  The suspending version reproduces that deliberately,
    // because diverging would be worse; neither is obviously correct.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
