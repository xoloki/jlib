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
 * util_http_async_test -- the first framing function that suspends.
 *
 * The subject is not "does it read a head" -- the synchronous one has done
 * that for a while -- but **do the two agree**, including about what they
 * refuse and what they say when they refuse it.  A coroutine that reads the
 * happy path and diverges on a truncated message is worse than no coroutine.
 */

#include "feed.hh"

#include <jlib/sys/async_reader.hh>
#include <jlib/sys/await.hh>
#include <jlib/sys/pipe.hh>
#include <jlib/sys/reactor.hh>
#include <jlib/util/http.hh>

#include <unistd.h>

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace sys = jlib::sys;
namespace http = jlib::util::http;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** What the synchronous one does with this input: a value, or a message. */
struct outcome {
    bool threw = false;
    std::string value;
    std::string why;

    bool operator==(const outcome& o) const {
        return threw == o.threw && value == o.value && why == o.why;
    }
};

static outcome sync_read(const std::string& in, std::size_t cap) {
    outcome o;
    std::istringstream is(in);

    try { o.value = http::read_head(is, cap); }
    catch(std::exception& e) { o.threw = true; o.why = e.what(); }

    return o;
}

/**
 * The same input through the coroutine, delivered in pieces.
 *
 * @param chunk how many octets at a time, so the reader is forced to suspend
 *              partway through -- which is the whole difference being tested.
 */
static outcome async_read(const std::string& in, std::size_t cap,
                          std::size_t chunk)
{
    outcome o;

    sys::reactor r;

    // Raw descriptors rather than sys::pipe, because the write end has to be
    // *closed* to make a truncated input arrive as end of stream, and pipe
    // owns both ends and closes them itself.
    int fds[2];

    if(::pipe(fds) != 0) { o.threw = true; o.why = "pipe() failed"; return o; }

    std::thread feeder([&]{
        for(std::size_t at = 0; at < in.size(); at += chunk) {
            const std::string part = in.substr(at, chunk);

            std::this_thread::sleep_for(std::chrono::milliseconds(2));

            feed(fds[1], part);
        }

        ::close(fds[1]);
    });

    sys::async_fd_reader in_r(r, fds[0]);
    sys::task<std::string> t = http::read_head(in_r, cap);

    try { o.value = sys::run_until_complete(r, t); }
    catch(std::exception& e) { o.threw = true; o.why = e.what(); }

    feeder.join();

    ::close(fds[0]);

    return o;
}

static void they_agree(const std::string& what, const std::string& input,
                       std::size_t cap = 8192)
{
    const outcome a = sync_read(input, cap);

    // One octet at a time is the worst case: a suspension between every pair
    // of characters, so every state of the loop is crossed by a co_await.
    const outcome b = async_read(input, cap, 1);

    // And in one go, where the buffer never runs dry and fill() is awaited
    // once.
    const outcome c = async_read(input, cap, input.empty() ? 1 : input.size());

    ok("  " + what, a == b && a == c,
       a == b ? (a == c ? "" : "differs when delivered whole")
              : "differs when delivered one octet at a time");

    if(!(a == b)) {
        std::cout << "         sync : " << (a.threw ? "threw " + a.why : "\"" + a.value + "\"") << "\n";
        std::cout << "         async: " << (b.threw ? "threw " + b.why : "\"" + b.value + "\"") << "\n";
    }
}

static void the_two_read_heads_agree() {
    std::cout << "\nthe synchronous and suspending read_head agree:\n";

    they_agree("an ordinary request head",
               "GET / HTTP/1.1\r\nHost: example.org\r\n\r\n");

    they_agree("one with no fields", "GET / HTTP/1.1\r\n\r\n");

    they_agree("one with several fields",
               "POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n"
               "Accept: */*\r\n\r\n");

    // The bare-LF end, which both accept so that the grammar can refuse it
    // with a message rather than the cap running out.
    they_agree("a head ended with bare LFs", "GET / HTTP/1.1\nHost: a\n\n");

    they_agree("a head with a bare LF in the middle",
               "GET / HTTP/1.1\nHost: a\r\n\r\n");

    // The refusals, which matter more than the successes.
    they_agree("a truncated head", "GET / HTTP/1.1\r\nHost: exa");

    they_agree("nothing at all", "");

    they_agree("a head that never ends",
               std::string("GET / HTTP/1.1\r\n") + std::string(400, 'x'), 64);

    they_agree("one exactly at the cap",
               "GET / HTTP/1.1\r\n\r\n", 18);

    they_agree("one one octet over the cap",
               "GET / HTTP/1.1\r\n\r\n", 17);
}

// ---------------------------------------------------------------- read_body

static outcome sync_body(const std::string& in, http::framing how,
                         std::size_t length, std::size_t cap)
{
    outcome o;
    std::istringstream is(in);

    try { o.value = http::read_body(is, how, length, cap); }
    catch(std::exception& e) { o.threw = true; o.why = e.what(); }

    return o;
}

static outcome async_body(const std::string& in, http::framing how,
                          std::size_t length, std::size_t cap,
                          std::size_t chunk)
{
    outcome o;

    sys::reactor r;

    int fds[2];

    if(::pipe(fds) != 0) { o.threw = true; o.why = "pipe() failed"; return o; }

    std::thread feeder([&]{
        for(std::size_t at = 0; at < in.size(); at += chunk) {
            const std::string part = in.substr(at, chunk);

            std::this_thread::sleep_for(std::chrono::milliseconds(1));

            feed(fds[1], part);
        }

        ::close(fds[1]);
    });

    sys::async_fd_reader in_r(r, fds[0]);
    sys::task<std::string> t = http::read_body(in_r, how, length, cap);

    try { o.value = sys::run_until_complete(r, t); }
    catch(std::exception& e) { o.threw = true; o.why = e.what(); }

    feeder.join();

    ::close(fds[0]);

    return o;
}

static void bodies_agree(const std::string& what, const std::string& input,
                         http::framing how, std::size_t length = 0,
                         std::size_t cap = 1048576)
{
    const outcome a = sync_body(input, how, length, cap);

    // One octet at a time: a suspension between every pair of characters, so
    // every state of the chunked loop is crossed by a co_await -- mid size
    // line, mid chunk data, mid trailing CRLF, mid trailer.
    const outcome b = async_body(input, how, length, cap, 1);

    const outcome c = async_body(input, how, length, cap,
                                 input.empty() ? 1 : input.size());

    ok("  " + what, a == b && a == c,
       a == b ? (a == c ? "" : "differs when delivered whole")
              : "differs when delivered one octet at a time");

    if(!(a == b)) {
        std::cout << "         sync : "
                  << (a.threw ? "threw " + a.why : "\"" + a.value + "\"") << "\n";
        std::cout << "         async: "
                  << (b.threw ? "threw " + b.why : "\"" + b.value + "\"") << "\n";
    }
}

/**
 * The four framing cases, and the one that matters.
 *
 * read_head was the easy one -- a byte loop over a delimiter with nothing
 * carried across a suspension.  The chunked case interleaves *parsing* a size
 * with *reading* data, round after round, and one octet at a time puts a
 * co_await between every step of that.
 */
static void the_two_read_bodies_agree() {
    std::cout << "\nthe synchronous and suspending read_body agree:\n";

    bodies_agree("no body at all", "", http::framing::none);

    bodies_agree("a body of a stated length", "hello", http::framing::length, 5);

    bodies_agree("one that is short", "hel", http::framing::length, 5);

    bodies_agree("one longer than the cap", "hello", http::framing::length, 5, 4);

    bodies_agree("a body read to end of stream", "whatever arrives",
                 http::framing::until_close);

    bodies_agree("one read to close, past the cap", "0123456789",
                 http::framing::until_close, 0, 4);

    // The chunked cases.
    bodies_agree("one chunk", "5\r\nhello\r\n0\r\n\r\n", http::framing::chunked);

    bodies_agree("several chunks",
                 "3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n", http::framing::chunked);

    bodies_agree("a chunk extension, parsed off",
                 "5;a=b\r\nhello\r\n0\r\n\r\n", http::framing::chunked);

    bodies_agree("a trailer section, consumed",
                 "3\r\nabc\r\n0\r\nX-Thing: 1\r\n\r\n",
                 http::framing::chunked);

    // Data that looks like its own framing, which is the case an in-process
    // peer exists to produce and a real server never will on request.
    bodies_agree("chunk data that looks like a chunk header",
                 "9\r\n5\r\nhello\r\n\r\n0\r\n\r\n", http::framing::chunked);

    // The refusals.
    bodies_agree("a bad chunk size", "zz\r\nhello\r\n0\r\n\r\n",
                 http::framing::chunked);

    bodies_agree("a chunk that ends early", "5\r\nhel", http::framing::chunked);

    bodies_agree("a chunk not followed by CRLF", "5\r\nhelloXX0\r\n\r\n",
                 http::framing::chunked);

    bodies_agree("a bare LF in a chunked body", "5\nhello\r\n0\r\n\r\n",
                 http::framing::chunked);

    bodies_agree("a chunked body past the cap",
                 "10\r\n0123456789abcdef\r\n0\r\n\r\n",
                 http::framing::chunked, 0, 8);

    bodies_agree("a chunked body that just stops", "3\r\nabc\r\n",
                 http::framing::chunked);
}

/** The half that did not change: the parser, over what the coroutine produced. */
static void the_parser_is_untouched() {
    std::cout << "\nthe parser is untouched:\n";

    sys::reactor r;
    sys::pipe p(false, false);

    const std::string msg = "GET /thing?q=1 HTTP/1.1\r\nHost: example.org\r\n\r\n";

    feed(p.get_writer(), msg);

    sys::async_fd_reader in(r, p.get_reader());
    sys::task<std::string> t = http::read_head(in, 8192);

    const std::string head = sys::run_until_complete(r, t);

    // parse_request_head takes a string_view and is not a coroutine.  That
    // split is why this costs six functions rather than fifty-nine.
    const http::Request q = http::parse_request_head(head);

    ok("  the coroutine's output parses", q.method() == "GET", q.method());

    ok("  with the target intact", q.target() == "/thing?q=1", q.target());

    ok("  and the fields", jlib::util::http::fold(q.fields().get("Host")) ==
       "example.org", q.fields().get("Host"));
}

/** Cancellation reaches a framing function that knows nothing about it. */
static void a_framing_function_inherits_cancellation() {
    std::cout << "\na framing function inherits cancellation:\n";

    sys::reactor r;
    sys::pipe p(false, false);
    sys::cancel_token token = sys::cancel_token::create();

    // A head that never ends, from a peer that never says more.
    const std::string partial = "GET / HTTP/1.1\r\nHost: ex";

    feed(p.get_writer(), partial);

    sys::async_fd_reader in(r, p.get_reader(), token);
    sys::task<std::string> t = http::read_head(in, 8192);

    t.start();

    ok("  it consumed what was there and suspended", !t.done());

    token.request();

    // Something has to end the wait; a real caller cancels from a timer on
    // this thread.  See the closing note -- a requested token does not by
    // itself end a wait on a descriptor that never becomes ready.
    feed(p.get_writer(), "x", 1);

    bool cancelled = false;

    try { sys::run_until_complete(r, t); }
    catch(sys::cancelled&) { cancelled = true; }

    // read_head does not mention cancel_token anywhere.  It inherits it
    // through async_reader::fill, which inherits it from until_ready.
    ok("  and cancelling the reader cancels the read", cancelled);
}

// ---- a sink, and one that suspends (#242) ---------------------------------

/**
 * The same body through both sinks, with the async one **suspending inside
 * the sink**.
 *
 * That is the assertion the whole `task<bool>` signature exists for.  A sink
 * returning bool would compile, deliver the same bytes, and be unable to do
 * this -- and "unable to do this" is what a server streaming a request body
 * onward needs, so it would have been discovered by somebody trying to write
 * that, one design later.  See #218 for the same lesson learned the other way.
 */
static void a_sink_may_suspend() {
    std::cout << "\na sink that suspends between blocks:\n";

    const std::string wire = "6\r\nGET /a\r\n5\r\n HTTP\r\n0\r\n\r\n";

    sys::reactor r;

    int fds[2], gate[2];

    if(::pipe(fds) != 0 || ::pipe(gate) != 0) {
        ok("pipes", false);

        return;
    }

    std::thread feeder([&]{
        for(std::size_t at = 0; at < wire.size(); at += 3) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

            feed(fds[1], wire.substr(at, 3));
        }

        ::close(fds[1]);
    });

    sys::async_fd_reader in_r(r, fds[0]);

    std::string got;
    std::size_t suspensions = 0;

    // The sink writes a byte to a pipe and waits for it to come back readable
    // -- a real suspension, on the reactor, in the middle of reading a body.
    sys::task<void> t = http::read_body(
        in_r, http::framing::chunked, 0,
        [&](std::string_view piece) -> sys::task<bool> {
            got.append(piece);

            const char one = 'x';

            if(::write(gate[1], &one, 1) == 1) {
                co_await sys::readable(r, gate[0]);

                char back = 0;

                if(::read(gate[0], &back, 1) == 1) suspensions++;
            }

            co_return true;
        });

    bool threw = false;

    try { sys::run_until_complete(r, t); }
    catch(std::exception& e) { threw = true; got = e.what(); }

    feeder.join();

    ::close(fds[0]);
    ::close(gate[0]);
    ::close(gate[1]);

    ok("the body arrives through a sink that suspends", !threw && got == "GET /a HTTP",
       threw ? got : "\"" + got + "\"");

    ok("  and it really did suspend, once per block", suspensions >= 2,
       std::to_string(suspensions) + " suspensions");
}

/** The two sinks see the same octets, which is this file's whole subject. */
static void the_two_sinks_agree() {
    std::cout << "\nthe blocking and suspending sinks agree:\n";

    const std::string wire = "4\r\nabcd\r\n4\r\nefgh\r\n0\r\n\r\n";

    std::string sync_got;

    {
        std::istringstream is(wire);

        http::read_body(is, http::framing::chunked, 0,
                        [&](std::string_view piece) {
                            sync_got.append(piece);

                            return true;
                        });
    }

    sys::reactor r;

    int fds[2];

    if(::pipe(fds) != 0) { ok("pipe", false); return; }

    std::thread feeder([&]{ feed(fds[1], wire); ::close(fds[1]); });

    sys::async_fd_reader in_r(r, fds[0]);

    std::string async_got;

    // A temporary lambda, which is how anybody will call this -- and what
    // crashed until the sink was taken by value.
    sys::task<void> t = http::read_body(
        in_r, http::framing::chunked, 0,
        [&](std::string_view piece) -> sys::task<bool> {
            async_got.append(piece);

            co_return true;
        });

    sys::run_until_complete(r, t);

    feeder.join();

    ::close(fds[0]);

    ok("both sinks see the same body", sync_got == async_got && sync_got == "abcdefgh",
       sync_got + " vs " + async_got);
}

/** A sink that stops, which ends the read wherever it had got to. */
static void a_suspending_sink_may_stop() {
    std::cout << "\na suspending sink that says stop:\n";

    const std::string wire = "4\r\nabcd\r\n4\r\nefgh\r\n0\r\n\r\n";

    sys::reactor r;

    int fds[2];

    if(::pipe(fds) != 0) { ok("pipe", false); return; }

    std::thread feeder([&]{ feed(fds[1], wire); ::close(fds[1]); });

    sys::async_fd_reader in_r(r, fds[0]);

    std::string got;
    std::size_t calls = 0;

    sys::task<void> t = http::read_body(
        in_r, http::framing::chunked, 0,
        [&](std::string_view piece) -> sys::task<bool> {
            calls++;
            got.append(piece);

            co_return false;
        });

    sys::run_until_complete(r, t);

    feeder.join();

    ::close(fds[0]);

    ok("the read ends at the first refusal", calls == 1 && got == "abcd",
       std::to_string(calls) + " calls, \"" + got + "\"");
}

int main() {
    the_two_sinks_agree();
    a_sink_may_suspend();
    a_suspending_sink_may_stop();
    std::cout << std::unitbuf;

    the_two_read_heads_agree();
    the_two_read_bodies_agree();
    the_parser_is_untouched();
    a_framing_function_inherits_cancellation();

    // What a green run does not establish.
    //
    // That read_head is representative.  It is the *easy* framing function: a
    // byte loop over a delimiter with no state to carry across a suspension.
    // read_body has four cases and one of them interleaves parsing a chunk
    // size with reading chunk data; imap::read re-parses each line's tail to
    // find a literal length and then reads exactly that many octets.  Those
    // are where the shape will hold or not, and neither is converted.
    //
    // That anything in jlib awaits this.  Nothing does.  net::http::server
    // still calls the synchronous read_head, and both remain.
    //
    // That a requested token ends a wait.  It does not: the await stays
    // registered until the descriptor happens to become ready, which is why
    // the section above has to write a byte to provoke it.  A slow-loris
    // defence needs the wait to end on the token, and that is the next piece
    // of cancellation.
    //
    // And nothing about TLS.  async_reader reads a descriptor; a TLS stream
    // answers a second readiness question -- whether the SSL holds buffered
    // plaintext -- that a descriptor cannot, and #4 records that as the real
    // work rather than something a reader wraps its way out of.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
