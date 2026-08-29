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

#ifndef JLIB_NET_HTTP_SERVER_HH
#define JLIB_NET_HTTP_SERVER_HH

#include <jlib/net/http.hh>

#include <jlib/sys/server.hh>
#include <jlib/sys/tls.hh>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jlib {
namespace net {
namespace http {

/**
 * Caps and the Server field.
 *
 * At namespace scope beside sys::server_policy, and for the same reason: a
 * default argument is a complete-class context, so `const options& o =
 * options()` on a member of the class that nests options does not compile.  An
 * earlier version routed around it with a second constructor.  Defining the
 * struct first removes the problem, and server::options still spells.
 */
struct server_options {
    std::size_t max_head = 8192;
    std::size_t max_body = 1 << 20;
    std::string server_name = "jlib/1.2";
};

/**
 * An HTTP/1.1 server.
 *
 * Separate from http.hh, which is a client and says at length that it is a
 * deliberately narrow one.  Two ends of a protocol in one header would muddy
 * that claim, and every existing client caller would start including
 * <jlib/sys/server.hh>, <thread> and OpenSSL to get it.
 *
 * ## As narrow as the client, and for the same reasons
 *
 * One request per connection and `Connection: close`, always.  No keep-alive:
 * the branch that built the message layer refused it because keep-alive framing
 * is where request smuggling lives, and a server has strictly more to lose there
 * than a client.  No streaming: a response is accumulated whole and written in
 * one go, which is what lets a handler that throws still be answered with a 500,
 * and which means a body has to fit in memory.
 *
 * It exists to receive an OAuth2 redirect on loopback and to be a test harness.
 * **It is not hardened for a public port** -- see the note on sys::server.
 */
class server {
public:
    using error = jlib::util::http::error;
    using fields = jlib::util::http::fields;
    using Request = jlib::util::http::Request;

    /**
     * A response under construction.
     *
     * Accumulated rather than streamed, so nothing has reached the socket until
     * the handler has returned -- which is why a handler that throws can still
     * be answered.
     */
    class response {
    public:
        response& status(int code, const std::string& reason = "");
        response& field(std::string name, std::string value);
        response& type(const std::string& content_type);
        response& body(std::string body);

        int status() const { return m_status; }
        const http::fields& fields() const { return m_fields; }
        const std::string& body() const { return m_body; }

        /** The octets, head and body, as they go on the wire. */
        std::string str(const std::string& server_name) const;

    private:
        int m_status = 200;
        std::string m_reason;
        http::fields m_fields;
        std::string m_body;
    };

    typedef std::function<void(const Request&, response&)> handler;

    using options = server_options;

    /**
     * Bind and serve.  Loopback by default.
     *
     * @param tls  a server context makes it https://; an empty one leaves it
     *             plaintext
     * @param p    threads, timeouts and the queue depth, all fixed here
     */
    server(unsigned short port = 0,
           const std::string& host = "127.0.0.1",
           const sys::tls_context& tls = sys::tls_context(),
           const sys::server::policy& p = sys::server::policy(),
           const options& o = options());

    ~server();

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    unsigned short port() const;
    bool tls() const;

    /** http:// or https://, with the port, and that path. */
    std::string url(const std::string& path = "/") const;

    /**
     * Route an exact method and an exact path.
     *
     * The path is the target's path component with the query removed.  Matching
     * is exact: no prefixes, no wildcards, no trailing-slash equivalence, and
     * routes are tried in the order they were added so a later one can never
     * shadow an earlier one.
     *
     * Percent-encoding *is* decoded first, because RFC 3986 2.1 makes "%65" and
     * "e" the same character -- but a target whose path contains an encoded
     * separator ("%2F") is refused rather than decoded, since decoding one
     * would change how many segments the path has and that is the shape of a
     * traversal bug.
     *
     * A GET route does not answer HEAD.  Register both, or use otherwise().
     */
    void route(const std::string& method, const std::string& path, handler h);

    /** What runs when no route matched.  The default answers 404. */
    void otherwise(handler h);

    bool serve_one(double timeout = 0);
    /** serve_one() until stop().  One-shot; see sys::server::run(). */
    void run();
    void stop();
    void join();

    /** The transport underneath: its policy, its peers, its on_error. */
    sys::server& transport();

private:
    void serve(sys::socketstream& s, const sys::peer& from);

    struct entry {
        std::string method;
        std::string path;
        handler run;
    };

    options m_options;
    std::vector<entry> m_routes;
    handler m_otherwise;
    std::unique_ptr<sys::server> m_transport;
    bool m_tls = false;
};

}
}
}

#endif // JLIB_NET_HTTP_SERVER_HH
