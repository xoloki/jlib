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

#ifndef JLIB_TESTS_HTTPSERVER_HH
#define JLIB_TESTS_HTTPSERVER_HH

// An HTTP server that answers whatever it is told to, in this process.
//
// For the cases a real server will not produce on request: a chunked body
// whose chunk data contains a line that looks like a chunk header, a response
// carrying both Content-Length and Transfer-Encoding, a Location: pointing
// somewhere it must not be followed to.  Those are the shapes that break a
// client, and no amount of asking nginx nicely will get them.
//
// It also records every request it was sent, which is how a test asserts what
// the *client* did -- that the query string reached the request-target, that
// Authorization came off across an origin change, that a POST body was not
// re-sent to a redirect.
//
// It speaks TLS when handed a certificate, which is the only way jlib gets an
// HTTPS test that runs on a developer's machine: net_http_live_test needs an
// nginx and there is not one outside the build container.
//
// This used to be about two hundred and fifty lines, most of them raw OpenSSL:
// its own SSL_CTX, its own SSL_accept, its own SSL_read and SSL_write, its own
// descriptor ownership, and a byte-at-a-time head reader -- because
// basic_tlsbuf could only call SSL_connect and there was no server-side
// counterpart in the library to reach for.  Its own header said so, and called
// it a gap this test worked around rather than one it fixed.  That gap is #112
// and it is closed; what is left here is sys::server plus a handler.
//
// It stays on sys::server rather than net::http::server on purpose.  Its whole
// job is to send responses a correct server would not -- both framing fields at
// once, a chunk header inside chunk data, a Content-Length that lies -- and a
// response class that computes its own Content-Length cannot produce those.
// Those are the assertions that matter most in net_http_test.
//
// It is not a substitute for a real server and net_http_live_test is the other
// half: a server nobody wrote sends responses nobody thought of.  This one
// sends exactly what somebody thought of, which is the point of it and also
// its whole limitation.

#include <jlib/sys/listener.hh>
#include <jlib/sys/server.hh>
#include <jlib/sys/socketstream.hh>
#include <jlib/sys/tls.hh>

#include <jlib/util/http.hh>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace httpserver {

/** One request as it arrived. */
struct request {
    std::string head;   ///< the request line and fields, CRLFs and all
    std::string body;
};

class server {
public:
    /** Given the request, return the raw octets to send back. */
    typedef std::function<std::string(const request&)> handler;

    /**
     * @param h     what to answer with
     * @param cert  a PEM certificate, or "" for a plaintext server
     * @param key   its private key
     */
    explicit server(handler h,
                    const std::string& cert = std::string(),
                    const std::string& key = std::string())
        : m_handler(std::move(h))
    {
        jlib::sys::tls_context ctx;

        if(!cert.empty()) ctx = jlib::sys::tls_context::server(cert, key);

        m_tls = !ctx.empty();

        m_server.reset(new jlib::sys::server(
            jlib::sys::listener(0, "127.0.0.1"),
            [this](jlib::sys::socketstream& s, const jlib::sys::peer&) {
                serve(s);
            },
            ctx));

        // Every connection is one a test made; a failure on one is not
        // something a test wants on its standard error unless it asks.
        m_server->on_error([](const std::exception&, const jlib::sys::peer&) {});

        m_thread = std::thread([this] { m_server->run(); });
    }

    ~server() { stop(); }

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    unsigned short port() const { return m_server->port(); }

    bool tls() const { return m_tls; }

    /**
     * Where to reach it.
     *
     * The TLS form says "localhost" rather than 127.0.0.1, because that is the
     * name the generated certificate covers and jlib checks it with
     * SSL_set1_host.
     */
    std::string url(const std::string& path = "/") const {
        return (m_tls ? "https://localhost:" : "http://127.0.0.1:") +
               std::to_string(port()) + path;
    }

    /** Every request served so far. */
    std::vector<request> requests() const {
        std::lock_guard<std::mutex> lock(m_lock);

        return m_requests;
    }

    void stop() {
        if(m_server) m_server->stop();

        if(m_thread.joinable()) m_thread.join();
    }

private:
    void serve(jlib::sys::socketstream& s) {
        request r;

        // read_head throws where the hand-rolled byte loop this replaced just
        // stopped -- which is what used to record a connection carrying no
        // complete head as though it were a request, and failed make check.
        r.head = jlib::util::http::read_head(s, 65536);

        const jlib::util::http::Request q =
            jlib::util::http::parse_request_head(r.head);

        r.body = jlib::util::http::read_body(s, q.body_framing(),
                                             q.content_length(), 1 << 20);

        std::string reply;

        {
            std::lock_guard<std::mutex> lock(m_lock);

            m_requests.push_back(r);
        }

        reply = m_handler(r);

        s << reply << std::flush;
    }

    handler m_handler;
    bool m_tls = false;
    mutable std::mutex m_lock;
    std::vector<request> m_requests;
    std::unique_ptr<jlib::sys::server> m_server;
    std::thread m_thread;
};

/** A minimal well-formed response. */
inline std::string reply(int status, const std::string& reason,
                         const std::string& body = "",
                         const std::string& extra = "")
{
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" +
           "Content-Length: " + std::to_string(body.size()) + "\r\n" +
           extra +
           "\r\n" + body;
}

}

#endif // JLIB_TESTS_HTTPSERVER_HH
