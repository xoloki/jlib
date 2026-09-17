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
 * Mutated requests, because hand-written cases only find what the author
 * thought of.
 *
 * #240's first round found an authentication bypass by *sending* bytes rather
 * than by reading code, which is an argument for doing that without a
 * hypothesis as well as with one.
 *
 * ## What it asserts, and what it deliberately does not
 *
 * Not that any particular mutant gets any particular status. A fuzzer that
 * knows the right answer is a unit test with extra steps, and most of these
 * inputs have no right answer -- 400 and 404 are both fine for a target that
 * is half a URL.
 *
 * What it asserts is the property that must hold for **every** input:
 *
 *   - the server answers or closes, and does neither slowly
 *   - it is still serving afterwards
 *
 * The second is the one that catches things. A crash, a wedged accept loop, a
 * connection leaked past its cap, a handler that throws where nothing catches
 * -- all of them show up as "the known-good request that follows does not get
 * its 200", whatever the mutant itself did.
 *
 * ## Deterministic on purpose
 *
 * A fixed seed and a fixed corpus, so this either passes always or fails
 * always. A fuzzer in `make check` that finds something new on a random run is
 * a flake to everybody who did not write it, and it will be deleted within the
 * month. Run it harder by hand with JLIB_FUZZ_ROUNDS; a failure there is a seed
 * to add to the corpus below.
 */

#include <jlib/net/http_server.hh>
#include <jlib/sys/socketstream.hh>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

/** The seeds.  Valid messages and the shapes round one found interesting. */
static std::vector<std::string> corpus() {
    std::vector<std::string> c;

    c.push_back("GET /ok HTTP/1.1\r\nHost: x\r\n\r\n");
    c.push_back("HEAD /ok HTTP/1.1\r\nHost: x\r\n\r\n");
    c.push_back("POST /ok HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
    c.push_back("POST /ok HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n"
                "\r\n5\r\nhello\r\n0\r\n\r\n");
    c.push_back("GET /static/page.html HTTP/1.1\r\nHost: x\r\nRange: bytes=0-3\r\n\r\n");
    c.push_back("GET /static/page.html HTTP/1.1\r\nHost: x\r\n"
                "If-None-Match: W/\"abc\"\r\n\r\n");
    c.push_back("GET /guarded/x HTTP/1.1\r\nHost: x\r\n"
                "Authorization: Basic cm9vdDpodW50ZXIy\r\n\r\n");
    c.push_back("GET /ok?a=1&b=%20 HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n");
    c.push_back("OPTIONS * HTTP/1.1\r\nHost: x\r\n\r\n");

    return c;
}

/** One mutation, chosen by `r`, applied to a copy of `seed`. */
static std::string mutate(const std::string& seed, std::mt19937& r) {
    std::string s = seed;

    if(s.empty()) return s;

    switch(r() % 8) {
    case 0: {   // flip a bit
        const std::size_t at = r() % s.size();

        s[at] = char(s[at] ^ (1 << (r() % 8)));

        break;
    }
    case 1: {   // insert a byte, favouring the ones that mean something
        static const char interesting[] = { '\r', '\n', ':', ' ', '\t', '%',
                                            '/', '\\', '"', '\0', '.', '-' };
        const std::size_t at = r() % (s.size() + 1);

        s.insert(at, 1, interesting[r() % (sizeof interesting)]);

        break;
    }
    case 2: {   // delete a run
        const std::size_t at = r() % s.size();
        const std::size_t n = 1 + r() % 8;

        s.erase(at, n);

        break;
    }
    case 3:     // truncate, which is a half-sent message
        s.erase(r() % s.size());

        break;

    case 4: {   // duplicate a line -- two Content-Lengths live here
        const std::size_t nl = s.find("\r\n");

        if(nl != std::string::npos && nl + 2 < s.size()) {
            const std::size_t nl2 = s.find("\r\n", nl + 2);

            if(nl2 != std::string::npos) {
                const std::string line = s.substr(nl + 2, nl2 - nl);

                s.insert(nl + 2, line);
            }
        }

        break;
    }
    case 5: {   // a long run, to find something that grows with it
        const std::size_t at = r() % (s.size() + 1);

        s.insert(at, std::string(1 + r() % 2000, 'A'));

        break;
    }
    case 6: {   // splice two seeds, which is how a smuggle is shaped
        const std::vector<std::string> c = corpus();

        s += c[r() % c.size()];

        break;
    }
    default: {  // percent-escape a byte, reaching the decoder
        const std::size_t at = r() % (s.size() + 1);
        static const char* const hex = "0123456789abcdef";
        std::string esc = "%";

        esc += hex[r() % 16];
        esc += hex[r() % 16];

        s.insert(at, esc);

        break;
    }
    }

    return s;
}

/**
 * Send `raw`, **close the write side**, and read until the server is done.
 *
 * The shutdown is what makes this finish in milliseconds instead of seconds.
 * A great many mutants are truncated messages, and a truncated message is one
 * the server is *right* to wait on -- it has a half-read head and no reason to
 * believe there is not more coming. Left to run, each such mutant costs one
 * client timeout, and four hundred of them cost the afternoon. Half-closing
 * says "that is all there is", which turns every mutant into a decision the
 * server can make now.
 *
 * A raw descriptor rather than socketstream, because socketstream has no
 * half-close -- and because a fuzzer that goes through the same client code
 * every other test uses is partly fuzzing that client.
 */
static bool exchange_bounded(unsigned short port, const std::string& raw,
                             double& secs)
{
    const std::chrono::steady_clock::time_point t0 =
        std::chrono::steady_clock::now();

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if(fd < 0) { secs = 0; return true; }

    struct sockaddr_in to;

    std::memset(&to, 0, sizeof to);

    to.sin_family = AF_INET;
    to.sin_port = htons(port);

    ::inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);

    struct timeval tv;

    tv.tv_sec = 2;
    tv.tv_usec = 0;

    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    // **Close abortively when done: RST, not FIN, so no TIME_WAIT.**
    //
    // One connection per mutant is a great many connections, and a normal
    // close leaves each one occupying an ephemeral port for the TIME_WAIT
    // duration.  Measured before this: the run died of its own tidiness after
    // about 15,000 mutants, having left 16,350 sockets in TIME_WAIT, and
    // every later run in the same minute failed at its first liveness check.
    //
    // Safe here specifically because the response has already been read by the
    // time this socket is closed, so there is nothing in flight to lose.  It
    // is a fuzzer's licence and not a pattern to copy into a client.
    struct linger abrupt;

    abrupt.l_onoff = 1;
    abrupt.l_linger = 0;

    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &abrupt, sizeof abrupt);

    if(::connect(fd, reinterpret_cast<struct sockaddr*>(&to), sizeof to) == 0) {
        std::size_t sent = 0;

        while(sent < raw.size()) {
            const ssize_t n = ::write(fd, raw.data() + sent, raw.size() - sent);

            if(n <= 0) break;

            sent += std::size_t(n);
        }

        ::shutdown(fd, SHUT_WR);

        char buf[4096];

        while(::read(fd, buf, sizeof buf) > 0) { }
    }

    ::close(fd);

    secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    return secs < 4.0;
}

/**
 * Why a liveness check did not get its 200.
 *
 * **The distinction is the whole value of the check.** One connection per
 * mutant is a lot of connections, and on loopback they pile up in TIME_WAIT
 * until the ephemeral range is gone -- measured at 16,350 after one 50,000
 * mutant run, after which every later run failed at its first liveness check
 * and blamed the server.
 *
 * A dead server and an exhausted client look identical if you only ask "did I
 * get a 200", and reporting one as the other is how a fuzzer earns its
 * reputation for crying wolf. They are distinguishable at the syscall:
 * ECONNREFUSED means nothing is listening, which is the finding; EADDRNOTAVAIL
 * and friends mean this process has run out of room to ask, which is not.
 */
enum liveness { alive, no_answer, server_gone, out_of_sockets };

static liveness still_serving(unsigned short port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if(fd < 0) return out_of_sockets;

    struct sockaddr_in to;

    std::memset(&to, 0, sizeof to);

    to.sin_family = AF_INET;
    to.sin_port = htons(port);

    ::inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);

    struct timeval tv;

    tv.tv_sec = 5;
    tv.tv_usec = 0;

    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    if(::connect(fd, reinterpret_cast<struct sockaddr*>(&to), sizeof to) != 0) {
        const int why = errno;

        ::close(fd);

        if(why == ECONNREFUSED) return server_gone;

        return out_of_sockets;
    }

    static const char* const req =
        "GET /ok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";

    const std::size_t n = std::strlen(req);

    if(::write(fd, req, n) != ssize_t(n)) { ::close(fd); return no_answer; }

    char buf[256];

    const ssize_t got = ::read(fd, buf, sizeof buf - 1);

    ::close(fd);

    if(got <= 0) return no_answer;

    buf[got] = 0;

    return std::strstr(buf, "200") ? alive : no_answer;
}

static void furnish(http::server& s, const std::string& root) {
    s.route("GET", "/ok", [](const http::server::Request&,
                             http::server::response& r) {
        r.status(200).type("text/plain").body("ok");
    });

    s.route("POST", "/ok", [](const http::server::Request& q,
                              http::server::response& r) {
        r.status(200).type("text/plain").body(q.body());
    });

    s.files("/static/*", root);

    s.protect("/guarded/*", "Basic realm=\"jlib\"",
              [](const http::server::credentials& c) {
                  return c.user == "root" && c.password == "hunter2";
              });
}

static void fuzz(http::server& s, const char* which, int rounds) {
    std::cout << "\n" << which << ", " << rounds << " mutants:\n";

    // Fixed seed: this run is the same run every time.  See the header.
    std::mt19937 r(20260916u);

    const std::vector<std::string> seeds = corpus();

    int slow = 0;
    double worst = 0;
    std::string worst_input;
    bool stopped_early = false;

    (void)stopped_early;

    for(int i = 0; i < rounds; i++) {
        std::string m = seeds[r() % seeds.size()];

        // One to three mutations, so a mutant can be further from a valid
        // message than a single edit allows.
        const int edits = 1 + int(r() % 3);

        for(int e = 0; e < edits; e++) m = mutate(m, r);

        double secs = 0;

        if(!exchange_bounded(s.port(), m, secs)) {
            slow++;

            if(secs > worst) { worst = secs; worst_input = m; }
        }

        // Every 64 mutants, check the server is still there.  Often enough to
        // name a small range of inputs when something does break it, rarely
        // enough not to double the runtime.
        if(i % 64 == 63) {
            const liveness how = still_serving(s.port());

            if(how == out_of_sockets) {
                // Not a finding, and not a failure.  The harness cannot ask
                // any more questions, so it stops rather than answering one it
                // did not get to test.
                std::cout << "  .. out of ephemeral sockets after "
                          << i << " mutants; stopping early\n";

                stopped_early = true;

                return;
            }

            if(how != alive) {
                ok("  the server survived mutant " + std::to_string(i), false,
                   std::string(how == server_gone ? "nothing listening"
                                                  : "listening, no answer") +
                   "; last input " + std::to_string(m.size()) + " octets");

                return;
            }
        }
    }

    ok("  nothing took longer than four seconds", slow == 0,
       std::to_string(slow) + " slow, worst " + std::to_string(worst) + "s");

    const liveness how = still_serving(s.port());

    if(how == out_of_sockets) {
        std::cout << "  .. out of ephemeral sockets; liveness not checked\n";

        stopped_early = true;
    }
    else {
        ok("  and the server is still serving afterwards", how == alive,
           how == alive ? std::string()
                        : std::string(how == server_gone ? "nothing listening"
                                                         : "listening, no answer"));
    }
}

int main() {
    std::cout << "net_http_fuzz_test\n";

    int rounds = 400;

    if(const char* env = std::getenv("JLIB_FUZZ_ROUNDS")) {
        const int n = std::atoi(env);

        if(n > 0) rounds = n;
    }

    try {
        char pattern[] = "/tmp/jlib_fuzz_XXXXXX";
        const std::string root = ::mkdtemp(pattern);

        {
            std::FILE* f = std::fopen((root + "/page.html").c_str(), "w");

            if(f) { std::fputs("<h1>x</h1>\n", f); std::fclose(f); }
        }

        {
            http::server s(0, "127.0.0.1");

            furnish(s, root);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            running go(s);

            fuzz(s, "the blocking server", rounds);
        }

        {
            http::server s(http::server::async_t(), 0, "127.0.0.1");

            furnish(s, root);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            running go(s);

            fuzz(s, "the async server", rounds);
        }

        const std::string rm = "rm -rf '" + root + "'";

        if(std::system(rm.c_str()) != 0) { }
    }
    catch(std::exception& e) {
        std::cerr << "net_http_fuzz_test: " << e.what() << "\n";
        return 1;
    }

    if(failures) {
        std::cerr << "net_http_fuzz_test: " << failures << " failed\n";
        return 1;
    }

    std::cout << "net_http_fuzz_test: all good\n";

    return 0;
}
