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
 * net_http_async_server_test -- the HTTP server, served on a reactor.
 *
 * The question is the one every conversion in #4 has asked: **do the two
 * agree**.  A buffered handler never suspends, so the *same* route table is
 * registered on both servers and every request is sent to both -- a difference
 * between them is a difference in what jlib answers, which is the only thing
 * that matters to a client.
 */

#include "certificate.hh"

#include <jlib/net/http.hh>
#include <jlib/net/http_server.hh>
#include <jlib/sys/await.hh>
#include <jlib/util/URL.hh>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace http = jlib::net::http;
namespace sys = jlib::sys;
namespace util = jlib::util;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** The routes both servers get, none of which is a coroutine. */
static void furnish(http::server& s) {
    s.route("GET", "/hello", [](const http::server::Request&,
                                http::server::response& r) {
        r.status(200).type("text/plain").body("hello from jlib\n");
    });

    s.route("GET", "/echo", [](const http::server::Request& q,
                               http::server::response& r) {
        r.status(200).type("text/plain").body(q.target());
    });

    s.route("POST", "/form", [](const http::server::Request& q,
                                http::server::response& r) {
        r.status(200).type("text/plain").body(q.body());
    });

    s.route("GET", "/boom", [](const http::server::Request&,
                               http::server::response&) {
        throw std::runtime_error("a handler failed");
    });
}

/** What a client saw: everything a caller could branch on. */
struct answer {
    int status = 0;
    std::string body;
    std::string type;
    std::string connection;
    bool threw = false;

    bool operator==(const answer& o) const {
        return status == o.status && body == o.body && type == o.type &&
            connection == o.connection && threw == o.threw;
    }
};

static answer fetch(http::server& s, const std::string& path,
                    const std::string& form = "")
{
    answer a;

    try {
        const util::URL u(s.url(path));

        const util::http::Response r = form.empty()
            ? http::get(u)
            : http::post_form(u, std::map<std::string, std::string>{
                                     { "k", form } });

        a.status = r.status();
        a.body = r.body();
        a.type = util::http::fold(r.fields().get("Content-Type"));
        a.connection = util::http::fold(r.fields().get("Connection"));
    }
    catch(std::exception&) { a.threw = true; }

    return a;
}

/** One server of each, the same routes, the same requests. */
static void the_two_servers_agree() {
    std::cout << "\nthe blocking and async servers agree:\n";

    http::server blocking(0, "127.0.0.1");
    http::server async(http::server::async_t(), 0, "127.0.0.1");

    furnish(blocking);
    furnish(async);

    blocking.transport().on_error([](const std::exception&, const sys::peer&) {});
    async.transport().on_error([](const std::exception&, const sys::peer&) {});

    std::thread bt([&blocking]{ blocking.run(); });
    std::thread at([&async]{ async.run(); });

    struct probe { const char* what; const char* path; const char* form; };

    const probe probes[] = {
        { "a plain GET",                 "/hello",           "" },
        { "the target reaches the handler", "/echo?a=1&b=2",  "" },
        { "a route that does not exist", "/nope",            "" },
        { "a handler that throws",       "/boom",            "" },
        { "an encoded path separator",   "/he%2fllo",        "" },
        { "a POST with a body",          "/form",            "value" },
    };

    for(const probe& p : probes) {
        const answer a = fetch(blocking, p.path, p.form);
        const answer b = fetch(async, p.path, p.form);

        ok(std::string("  ") + p.what, a == b,
           a == b ? "" : "blocking " + std::to_string(a.status) + " \"" +
                         a.body + "\" vs async " + std::to_string(b.status) +
                         " \"" + b.body + "\"");
    }

    blocking.stop();
    async.stop();

    bt.join();
    at.join();
}

/**
 * A request head dribbled in one octet at a time.
 *
 * Where the async server differs from the blocking one: a suspension falls
 * between every pair of characters in the head, so every state of the reader
 * is crossed by a co_await.  The blocking server reads the same bytes through
 * one istream and never suspends at all.
 */
static void a_head_delivered_one_octet_at_a_time() {
    std::cout << "\na head delivered one octet at a time:\n";

    http::server s(http::server::async_t(), 0, "127.0.0.1");

    furnish(s);
    s.transport().on_error([](const std::exception&, const sys::peer&) {});

    std::thread t([&s]{ s.run(); });

    sys::socketstream c("127.0.0.1", s.port(), -1, 10.0);

    const std::string head = "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";

    for(std::size_t i = 0; i < head.size(); i++) {
        c << head[i] << std::flush;

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::string status;

    std::getline(c, status);

    if(!status.empty() && status.back() == '\r') status.pop_back();

    ok("  it is answered", status == "HTTP/1.1 200 OK", status);

    s.stop();
    t.join();
}

/** A streaming handler, which is the one kind that has to be written anew. */
static sys::task<void> ticker(const http::server::Request&,
                              http::server::async_responder& out)
{
    http::server::response head;

    head.status(200).type("text/event-stream");

    co_await out.begin(head);

    for(int i = 0; i < 3; i++)
        co_await out.write("data: piece " + std::to_string(i) + "\n\n");
}

static void a_streaming_handler() {
    std::cout << "\na streaming handler:\n";

    http::server s(http::server::async_t(), 0, "127.0.0.1");

    s.route("GET", "/stream", http::server::async_stream_handler(ticker));
    s.transport().on_error([](const std::exception&, const sys::peer&) {});

    std::thread t([&s]{ s.run(); });

    const util::http::Response r = http::get(util::URL(s.url("/stream")));

    ok("  it answers 200", r.status() == 200, std::to_string(r.status()));

    ok("  as an event stream",
       util::http::fold(r.fields().get("Content-Type")) == "text/event-stream",
       r.fields().get("Content-Type"));

    ok("  with no Content-Length, because it was not known",
       !r.fields().has("Content-Length"));

    ok("  and every piece arrives, in order",
       r.body() == "data: piece 0\n\ndata: piece 1\n\ndata: piece 2\n\n",
       "\"" + r.body() + "\"");

    s.stop();
    t.join();
}

/** A route for the wrong kind of server says so rather than 404ing. */
static void a_route_for_the_other_kind_of_server() {
    std::cout << "\na route for the other kind of server:\n";

    {
        http::server blocking(0, "127.0.0.1");

        blocking.route("GET", "/stream",
                       http::server::async_stream_handler(ticker));

        blocking.transport().on_error([](const std::exception&,
                                         const sys::peer&) {});

        std::thread t([&blocking]{ blocking.run(); });

        const answer a = fetch(blocking, "/stream");

        ok("  a suspending route on a blocking server is a 500",
           a.status == 500, std::to_string(a.status));

        ok("  saying which way round it is",
           a.body.find("blocking") != std::string::npos, a.body);

        blocking.stop();
        t.join();
    }

    {
        http::server async(http::server::async_t(), 0, "127.0.0.1");

        async.route("GET", "/stream",
                    [](const http::server::Request&,
                       http::server::responder& out) {
                        http::server::response r;
                        r.status(200).body("x");
                        out.send(r);
                    });

        async.transport().on_error([](const std::exception&,
                                      const sys::peer&) {});

        std::thread t([&async]{ async.run(); });

        const answer a = fetch(async, "/stream");

        ok("  and a blocking route on an async server is too",
           a.status == 500, std::to_string(a.status));

        async.stop();
        t.join();
    }
}

int main() {
    std::cout << std::unitbuf;

    the_two_servers_agree();
    a_head_delivered_one_octet_at_a_time();
    a_streaming_handler();
    a_route_for_the_other_kind_of_server();

    // What a green run does not establish.
    //
    // Interoperability.  Both ends are jlib, as net_http_server_test says of
    // itself.  net_http_live_test is where jlib's *client* meets nginx and
    // there is still no equivalent for this direction, async or not.
    //
    // Nothing about TLS on this server.  sys_async_server_test drives the
    // transport's TLS branch with a byte-stream handler; nothing here puts
    // HTTP over it.
    //
    // Nothing about a slow-loris.  The pieces exist -- the connection is a
    // coroutine with a token, and a handler can arm a deadline -- and
    // serve_async arms nothing, so a client that connects and dribbles one
    // octet a minute holds a connection for as long as it likes.  The section
    // above proves the dribbling *works*, not that it is bounded.
    //
    // And nothing about concurrency here.  sys_async_server_test measures that
    // eight connections are in flight at once; this file has one client at a
    // time and says nothing about what happens under load.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
