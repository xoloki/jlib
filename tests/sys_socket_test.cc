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

// The four things basic_socketbuf could not do until now.
//
// It could connect, and that was all: no accepting end at all, an IPv4-only
// resolver, no deadline on connect(), and no way to bound a read.  Each section
// here is an assertion that could not have been written against the old code,
// in two cases because the API was missing and in two because the behaviour was
// "block until something else gives up".
//
// Entirely local -- a loopback listener in this same process -- except for the
// black-hole section, which needs an address that is guaranteed not to answer
// and takes RFC 5737's TEST-NET-1 for it.

#include <jlib/sys/listener.hh>
#include <jlib/sys/pipe.hh>
#include <jlib/sys/socketstream.hh>
#include <jlib/sys/sys.hh>

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

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

// A listener and a client connected to it, both in this process.
//
// No thread and no fork, because none is needed: the kernel completes the
// three-way handshake out of the listen backlog, so the connect returns before
// anything calls accept().  That is worth saying because it is the reason this
// test is not the fork-and-hope shape sys_tls_sigpipe_test has to use.
struct pair {
    jlib::sys::listener server;
    jlib::sys::socketstream client;
    std::unique_ptr<jlib::sys::socketstream> peer;

    pair() : server(), client("127.0.0.1", server.port()) {
        peer = server.accept_stream(5);
    }
};

static void a_listener_accepts_a_connection() {
    std::cout << "a listener accepts a connection\n";

    try {
        pair p;

        ok("the kernel picked a port", p.server.port() != 0,
           std::to_string(p.server.port()));

        if(!p.peer) {
            ok("accept returned a stream", false, "timed out");
            return;
        }

        ok("accept returned a stream", true);

        p.client << "from the client\n" << std::flush;

        std::string line;
        std::getline(*p.peer, line);
        ok("what the client wrote arrives at the server", line == "from the client", line);

        *p.peer << "and back again\n" << std::flush;

        std::getline(p.client, line);
        ok("and the other way", line == "and back again", line);
    }
    catch(std::exception& e) {
        ok("a loopback pair can be set up", false, e.what());
    }
}

static void a_connect_that_will_never_answer_gives_up() {
    std::cout << "a connect that will never answer gives up\n";

    // 192.0.2.0/24 is TEST-NET-1 (RFC 5737): reserved for documentation, so
    // nothing routes it and nothing answers.  Measured on a machine with no
    // deadline, a blocking connect(2) here takes 75 seconds to give up, which
    // is what the old code did on every call.
    //
    // The port is deliberately not 80.  A black hole is a property of the
    // port, not of the address: middleboxes intercept the ports they know --
    // consumer VPNs and captive portals both do it -- and answer the SYN
    // themselves for those, wherever they are addressed.  That was found here
    // on the first run of this test, over a commercial VPN, and measured:
    //
    //   80, 443, 8080, 53   connect in 40ms, to any address at all
    //   8000, 22, 993, 9999 time out, which is the real behaviour
    //   25                  refused outright (the VPN blocks outbound SMTP)
    //
    // Nothing is on the far end of the ones that connect.  Writing to
    // 192.0.2.1:80 succeeds and is then followed by twelve seconds of silence
    // and a clean EOF -- the middlebox accepts our handshake at once, goes off
    // to reach the destination, and closes when it cannot.  Which is a good
    // demonstration of why a connect deadline is not sufficient on its own and
    // why the last section here exists: those twelve seconds end in an eof()
    // that a caller with no timed_out() would read as a complete response.
    const auto start = std::chrono::steady_clock::now();
    bool threw = false;

    try {
        jlib::sys::socketstream sock("192.0.2.1", 9999, 2.0);
    }
    catch(std::exception&) {
        threw = true;
    }

    const double took = seconds_since(start);

    if(!threw) {
        // Somebody on the path is answering for an address that has no host,
        // so there is no black hole to time out against and nothing to assert.
        std::cout << "  skip  this network answers for TEST-NET-1; no black hole to test\n";
        return;
    }

    ok("it fails rather than connecting", threw);
    ok("and it does so within the timeout, not the kernel's", took < 10.0,
       std::to_string(took) + "s");
}

static void an_ipv6_literal_resolves() {
    std::cout << "an IPv6 literal resolves\n";

    // gethostbyname could not do this at all: it is AF_INET only, so "::1"
    // came back as an unresolvable name and every v6-only host was simply
    // unreachable from jlib.  Skipped rather than failed where the machine has
    // no IPv6, which some container networks do not.
    std::unique_ptr<jlib::sys::listener> server;

    try {
        server = std::make_unique<jlib::sys::listener>(0, "::1");
    }
    catch(std::exception& e) {
        std::cout << "  skip  no IPv6 loopback on this host: " << e.what() << "\n";
        return;
    }

    try {
        jlib::sys::socketstream client("::1", server->port());
        std::unique_ptr<jlib::sys::socketstream> peer = server->accept_stream(5);

        ok("a connection over IPv6 is accepted", peer != nullptr);

        if(peer) {
            client << "v6\n" << std::flush;

            std::string line;
            std::getline(*peer, line);
            ok("and carries data", line == "v6", line);
        }
    }
    catch(std::exception& e) {
        ok("a connection over IPv6 is accepted", false, e.what());
    }
}

static void a_listener_says_who_connected() {
    std::cout << "a listener says who connected\n";

    try {
        jlib::sys::listener server;
        jlib::sys::socketstream client("127.0.0.1", server.port());

        jlib::sys::peer who;
        std::unique_ptr<jlib::sys::socketstream> got = server.accept_stream(who, 5);

        ok("the connection is accepted", got != nullptr);

        // ::accept(m_sock, 0, 0) threw this away, so nothing could log or
        // refuse by who was on the other end.
        ok("and its address is there", who.address == "127.0.0.1", who.address);
        ok("with a port", who.port != 0, std::to_string(who.port));
        ok("and it knows loopback when it sees it", who.loopback());
    }
    catch(std::exception& e) {
        ok("the connection is accepted", false, e.what());
    }

    // The platform difference most likely to ship broken.  On BSD and macOS an
    // accepted descriptor *inherits* O_NONBLOCK from the listening socket; on
    // Linux it does not.  So a server that polls -- and therefore makes its
    // listener non-blocking -- would hand every handler on macOS a
    // non-blocking socket, and every read would fail with EAGAIN, while working
    // perfectly in the Linux container.  Asserted rather than discovered.
    try {
        jlib::sys::listener server;

        server.set_blocking(false);

        ok("a non-blocking listener accepts nothing when nothing is waiting",
           server.accept(0) == -1);

        jlib::sys::socketstream client("127.0.0.1", server.port());

        jlib::sys::peer who;
        const int fd = server.accept(who, 5);

        ok("but accepts a connection that is", fd >= 0);

        if(fd >= 0) {
            const int flags = ::fcntl(fd, F_GETFL, 0);

            ok("and the descriptor it hands back is blocking, on every platform",
               flags != -1 && !(flags & O_NONBLOCK),
               flags == -1 ? "fcntl failed" : "");

            ::close(fd);
        }
    }
    catch(std::exception& e) {
        ok("a non-blocking listener behaves", false, e.what());
    }
}

static void a_pipe_closes_what_it_opened() {
    std::cout << "a pipe closes what it opened\n";

    // The destructor deleted the descriptor array and left both files open, so
    // a process making pipes in a loop ran out of descriptors rather than
    // memory -- and Servent and ASServent each hold one for their lifetime.
    // Five thousand is well past any soft limit this would have hit.
    bool ran_out = false;
    std::string why;

    try {
        for(int i = 0; i < 5000; i++) {
            jlib::sys::pipe p;

            if(p.get_reader() < 0 || p.get_writer() < 0) {
                ran_out = true;
                break;
            }
        }
    }
    catch(std::exception& e) {
        ran_out = true;
        why = e.what();
    }

    ok("five thousand pipes come and go without exhausting the table",
       !ran_out, why);
}

static void a_read_can_be_bounded() {
    std::cout << "a read can be bounded\n";

    try {
        pair p;

        if(!p.peer) {
            ok("the pair is up", false, "accept timed out");
            return;
        }

        // The peer is connected and says nothing, which is what a hung server
        // looks like from here.  Without a timeout this getline never returns.
        p.client.set_timeout(0.5);

        const auto start = std::chrono::steady_clock::now();
        std::string line;
        std::getline(p.client, line);
        const double took = seconds_since(start);

        ok("the read returns", took < 5.0, std::to_string(took) + "s");
        ok("it took about as long as it was told to", took >= 0.4,
           std::to_string(took) + "s");
        ok("and says it timed out rather than that the peer closed",
           p.client.timed_out());

        // The distinction matters: a streambuf can only answer eof(), so
        // without timed_out() a caller has no way to tell a slow server from a
        // finished one, and would treat a stall as a clean end of response.
        ok("the connection is still open", p.peer->good());
    }
    catch(std::exception& e) {
        ok("a bounded read works", false, e.what());
    }
}

int main() {
    a_listener_accepts_a_connection();
    a_connect_that_will_never_answer_gives_up();
    an_ipv6_literal_resolves();
    a_listener_says_who_connected();
    a_pipe_closes_what_it_opened();
    a_read_can_be_bounded();

    // What a green run does not establish: that the connect timeout is
    // enforced by anything but poll(2) on this one host.  A firewall that
    // DROPs is the case it was written for and TEST-NET-1 is only a stand-in
    // for one -- on a network that returns ICMP unreachable for it, the
    // section passes without the deadline ever being reached.  Nor does it say
    // anything about SO_SNDTIMEO: bounding a write needs a peer that has
    // stopped reading and a full socket buffer, which is a megabyte of data
    // and a lot of patience for what it would prove.
    std::cout << (failures ? "FAILED" : "PASSED") << ": " << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
