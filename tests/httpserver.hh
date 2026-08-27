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
// It is not a substitute for a real server and net_http_live_test is the other
// half: a server nobody wrote sends responses nobody thought of.  This one
// sends exactly what somebody thought of, which is the point of it and also
// its whole limitation.

#include <jlib/sys/listener.hh>
#include <jlib/sys/socketstream.hh>

#include <jlib/util/http.hh>

#include <atomic>
#include <functional>
#include <istream>
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

    explicit server(handler h)
        : m_listener(),
          m_handler(std::move(h))
    {
        m_thread = std::thread([this] { run(); });
    }

    ~server() { stop(); }

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    unsigned short port() const { return m_listener.port(); }

    std::string url(const std::string& path = "/") const {
        return "http://127.0.0.1:" + std::to_string(m_listener.port()) + path;
    }

    /** Every request served so far. */
    std::vector<request> requests() const {
        std::lock_guard<std::mutex> lock(m_lock);

        return m_requests;
    }

    void stop() {
        m_stop = true;

        if(m_thread.joinable()) m_thread.join();

        m_listener.close();
    }

private:
    void run() {
        while(!m_stop) {
            std::unique_ptr<jlib::sys::socketstream> peer;

            try {
                // A short deadline rather than a blocking accept, so that
                // stopping does not depend on one more client turning up.
                peer = m_listener.accept_stream(0.2);
            }
            catch(std::exception&) {
                return;
            }

            if(!peer) continue;

            try {
                request r;

                r.head = jlib::util::http::read_head(*peer, 65536);

                // Only what a test sends: a Content-Length body, or none.
                const std::string::size_type at = find_ci(r.head, "\r\ncontent-length:");

                if(at != std::string::npos) {
                    const std::string::size_type eol = r.head.find("\r\n", at + 2);
                    const std::string value = r.head.substr(at + 17, eol - at - 17);
                    const std::size_t n = std::stoul(value);

                    r.body.resize(n);

                    if(n) peer->read(&r.body[0], static_cast<std::streamsize>(n));
                }

                std::string reply;

                {
                    std::lock_guard<std::mutex> lock(m_lock);

                    m_requests.push_back(r);
                }

                reply = m_handler(r);

                *peer << reply << std::flush;
            }
            catch(std::exception&) {
                // A client that hung up mid-request is a case some tests
                // create on purpose.
            }

            // The client sends Connection: close and so does this; closing
            // here is what ends a body with no framing fields on it.
            peer->close();
        }
    }

    static std::string::size_type find_ci(const std::string& hay, const std::string& needle) {
        std::string lower;

        for(char c : hay) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        return lower.find(needle);
    }

    jlib::sys::listener m_listener;
    handler m_handler;
    mutable std::mutex m_lock;
    std::vector<request> m_requests;
    std::atomic<bool> m_stop{false};
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
