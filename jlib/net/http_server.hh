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

#include <jlib/sys/async_writer.hh>
#include <jlib/sys/server.hh>
#include <jlib/sys/task.hh>
#include <jlib/sys/tls.hh>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jlib {
namespace net {
namespace http {

/** The default Server: "jlib/" and the release.  See options::server_name. */
std::string default_server_name();

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
    /**
     * What jlib calls itself on the wire.  See net::http::default_user_agent,
     * which this had the same staleness as: a literal in an installed header
     * cannot see the version.
     */
    std::string server_name = default_server_name();

    /**
     * Answer more than one request on a connection -- RFC 9112 9.3.
     *
     * **Asynchronous server only**, and on by default there, because that is
     * what HTTP/1.1 means: a client that has not said `Connection: close`
     * expects the connection to stay open, and a server that closes anyway
     * charges it a TCP handshake -- and a TLS one -- for every request.  A
     * peer that wants the old behaviour asks for it, which is exactly what
     * jlib's own client does and will keep doing.
     *
     * **What makes it safe is one layer down, and predates it.**  Keep-alive
     * framing is where request smuggling lives, and is why this was refused
     * until now.  The defence is that util::http::decide_framing already
     * throws on both primitives -- Content-Length together with
     * Transfer-Encoding, and two Content-Lengths that disagree -- so a message
     * whose end is arguable never reaches a handler; and that any framing
     * error closes the connection rather than looking for the next request in
     * a stream it has just said it cannot parse.
     */
    bool keep_alive = true;

    /**
     * How many requests one connection may ask for before it is closed.
     *
     * A bound, not a tuning knob: without one a single client holds a
     * descriptor -- and a slot against sys::server_policy::max_connections --
     * for as long as it keeps asking.
     */
    std::size_t max_requests = 100;

    /**
     * How long a *newly accepted* connection may stay silent, in seconds.
     *
     * A client that has just connected is about to say something -- in HTTP the
     * client always speaks first -- so one that connects and then says nothing
     * is not being slow, it is holding a descriptor for no reason.  Short on
     * purpose, and much shorter than the two bounds below.
     *
     * Note that this bounds *silence*, not the request: once the first octet
     * arrives the request timeout takes over, so a client that is genuinely
     * slow is not punished for being prompt.
     */
    double initial_idle_timeout = 5;

    /**
     * How long a *reused* connection may sit between requests, in seconds.
     *
     * **Long on purpose, and this is the number that decides whether keep-alive
     * does anything at all.**  A page that polls every fifteen seconds over a
     * connection with a five second idle bound gets a new connection every
     * time and no benefit whatsoever -- keep-alive that silently does nothing
     * is worse than keep-alive that is switched off, because it looks like a
     * feature.  Sixty seconds keeps such a client on one connection; nginx's
     * keepalive_timeout defaults to seventy-five for the same reason.
     *
     * The cost is held descriptors and slots against max_connections, which is
     * why it is a separate number from initial_idle_timeout rather than one
     * bound applied everywhere: a connection that has never asked for anything
     * has not earned the benefit of the doubt, and one that has, has.
     *
     * **Setting this close to a client's polling interval is the bad case**,
     * not setting it low.  Well below the interval means a fresh connection
     * every time, which merely wastes a handshake; near it means the server
     * decides to close at the same moment the client decides to ask, and the
     * client sees a connection die under a request it has already sent.
     */
    double idle_timeout = 60;
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
 * `Connection: close` after every response -- **except on the asynchronous
 * server, which answers more than one request per connection.**  The branch
 * that built the message layer refused keep-alive outright, because keep-alive
 * framing is where request smuggling lives and a server has strictly more to
 * lose there than a client.  What changed is not the risk assessment: it is
 * that the framing refusals which make the smuggle unreachable are now in
 * util::http, and that an idle persistent connection costs a coroutine here
 * where it would cost a thread in serve().  See server_options::keep_alive.
 *
 * A streaming response still closes, on either server.  Its body is delimited
 * by the close, so there is no boundary for a next message to begin at.
 *
 * A response is accumulated whole and written in one go, which is what lets a
 * handler that throws still be answered with a 500, and which means a body has
 * to fit in memory.  **Unless the handler asks otherwise** -- see `responder`,
 * added for server-sent events, where a body produced over seconds has to
 * reach the client as it appears.  That path gives up the 500, knowingly and
 * only for the handlers that take it.
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

        /**
         * The octets, head and body, as they go on the wire.
         *
         * @param persist  whether another message follows this one on the
         *                 connection, which decides the Connection field when
         *                 the handler has not set one.  Defaulted to false --
         *                 close -- so every caller that had no opinion keeps
         *                 the behaviour it had.
         */
        std::string str(const std::string& server_name,
                        bool persist = false) const;

        /**
         * The head alone, with no Content-Length.
         *
         * For a response whose length is not known when it starts; the body
         * follows from responder::write and is delimited by the close.
         */
        std::string head(const std::string& server_name) const;
        // No persist parameter, and there should not be one: this omits
        // Content-Length, so the close *is* the framing and the connection
        // cannot be reused whatever anybody would prefer.

    private:
        std::string serialise(const std::string& server_name,
                              bool with_length, bool persist) const;

        int m_status = 200;
        std::string m_reason;
        http::fields m_fields;
        std::string m_body;
    };

    typedef std::function<void(const Request&, response&)> handler;

    /**
     * A response written as it is produced, rather than accumulated.
     *
     * A buffered handler fills a `response` and the server sends it; that is
     * unchanged and is still the right shape for almost everything.  This is
     * for the case a buffered handler cannot serve at all: a body that is
     * produced over seconds and has to reach the client as it appears.
     *
     * Server-sent events are the case in hand.  A token stream at ninety
     * tokens a second is twenty seconds of silence and then a dump if it is
     * accumulated, and a coding harness reading it will have given up.
     *
     * ## What calling begin() gives up
     *
     * The status line goes out immediately, so it can no longer be changed.
     * A handler that throws afterwards **cannot** be answered with a 500 --
     * the 200 left long ago -- and the connection is closed instead, which the
     * client sees as a truncated body.
     *
     * That is precisely what the buffered path bought by accumulating, and it
     * is given up knowingly rather than by oversight.  A handler that can fail
     * should do its failing before it calls begin().
     *
     * ## Framing
     *
     * No `Content-Length`, because the length is not known, and
     * `Connection: close`, so the body is delimited by the close.  That is
     * legal HTTP/1.1 and it is what this server already does about
     * keep-alive; chunked encoding would buy nothing here and is another
     * framing to get wrong.
     */
    class responder {
    public:
        responder(sys::socketstream& s, const std::string& server_name)
            : m_s(&s), m_name(server_name) {}

        /** Send a complete response.  Exactly what a buffered handler does. */
        void send(const response& r);

        /**
         * Send the head now and commit the status.
         *
         * The response's body is ignored -- what follows comes from write().
         * `Content-Length` is omitted whatever the handler set, because a
         * length that later disagrees with the body is a framing bug and this
         * is the one place the server can be sure it would.
         */
        void begin(const response& head);

        /** One piece of the body, flushed. */
        void write(const std::string& piece);

        /** Whether anything has reached the socket. */
        bool started() const { return m_started; }

        /**
         * Whether the client is still there.
         *
         * A stream nobody is reading should stop being produced -- for a
         * token stream that means a model held for nothing.  False once a
         * write has failed.
         */
        bool live() const;

    private:
        sys::socketstream* m_s;
        std::string m_name;
        bool m_started = false;
    };

    /**
     * A handler that may stream.
     *
     * Registered by the same `route()`, overloaded on the signature.  Nothing
     * chooses between buffered and streaming *for* a handler: the request's
     * head and body are both read before routing, so a handler sees
     * `Accept: text/event-stream` and `"stream": true` in a JSON body alike
     * and decides for itself.  Which matters, because the OpenAI protocol puts
     * it in the body and clients disagree about sending the header.
     */
    typedef std::function<void(const Request&, responder&)> stream_handler;

    /**
     * A response written as it is produced, without blocking a thread.
     *
     * The suspending twin of responder.  Every operation returns a task
     * because a write can fill the socket buffer and have to wait for room,
     * and a server that blocked there would stall every other connection on
     * the reactor.
     *
     * The same contract otherwise, including the one that matters: once
     * begin() has gone out the status cannot change, so **a handler that
     * throws afterwards cannot be answered with a 500**.  http_server.hh has
     * said that about responder since #200 and it is no less true here.
     */
    class async_responder {
    public:
        async_responder(sys::async_writer& w, const std::string& server_name)
            : m_writer(&w), m_name(server_name) {}

        /** Send a complete response.  Exactly what a buffered handler does. */
        sys::task<void> send(const response& r);

        /**
         * The same, for a caller that has already serialised it.
         *
         * Which serve_async has, because it does that on a worker: str()
         * checks every field against the grammar and is not something to do
         * on the thread dispatching every other connection.
         */
        sys::task<void> send_serialised(const std::string& wire);

        /**
         * Send the head now and commit the status.
         *
         * Content-Length is omitted whatever the handler set, and the body is
         * delimited by the close -- see responder::begin, which explains why
         * chunked would buy nothing against a server that closes anyway.
         */
        sys::task<void> begin(const response& head);

        /** One piece of the body. */
        sys::task<void> write(const std::string& piece);

        bool started() const { return m_started; }

    private:
        sys::async_writer* m_writer;
        std::string        m_name;
        bool               m_started = false;
    };

    /**
     * A streaming handler that suspends.
     *
     * For a body produced over seconds -- server-sent events, a token stream.
     * A *buffered* handler needs none of this: it fills a response and the
     * server writes it, so the existing `handler` type works in async mode
     * unchanged, which is what lets the two modes be compared on the same
     * routes.
     */
    typedef std::function<sys::task<void>(const Request&, async_responder&)>
        async_stream_handler;

    /** Tag for the constructors that serve on a reactor.  See the class note. */
    struct async_t { explicit async_t() = default; };

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

    /**
     * The same, serving on a reactor rather than on threads.
     *
     * A connection is a coroutine suspended on the transport's reactor, so
     * policy::threads is not used and policy::max_connections is what bounds
     * concurrency instead.  See sys::server for the shape of that.
     *
     * **Buffered handlers work unchanged.**  A handler that fills a response
     * never suspends, so the same route table serves both modes -- which is
     * how the two are compared in net_http_server_test.  Only a *streaming*
     * handler has to be written differently, because only writing suspends.
     */
    server(async_t, unsigned short port = 0,
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

    /** The same, for a handler that may stream; see responder. */
    void route(const std::string& method, const std::string& path,
               stream_handler h);

    /**
     * The same, for a handler that suspends.  Only an async server can run one.
     *
     * A route registered this way on a blocking server throws when it is
     * reached, rather than at registration: a server may legitimately carry
     * routes it never serves, and refusing at registration would stop a
     * caller building one table for both modes.
     */
    void route(const std::string& method, const std::string& path,
               async_stream_handler h);

    /** What runs when no route matched.  The default answers 404. */
    void otherwise(handler h);

    bool serve_one(double timeout = 0);
    /** serve_one() until stop().  One-shot; see sys::server::run(). */
    void run();

    /**
     * Ask run(), and any serve_one() that is waiting, to return.
     *
     * @param drain  answer the requests already accepted but not yet started,
     *               or drop them unanswered.  Queued work only: a handler
     *               already running finishes either way.
     *
     * Passed straight to sys::server::stop(), which is where the whole story
     * is.  The part worth repeating here is that dropping a request answers it
     * with nothing -- not a 503, not a close-after-status, just a close -- so
     * false is for shutting down under duress and true is for shutting down.
     */
    void stop(bool drain = true);

    /**
     * Wait for the handler threads to retire.  **stop() first.**
     *
     * sys::server::join() does not stop on its own behalf and neither does
     * this, so a join() without a preceding stop() hangs -- with a pool.  With
     * the default policy there is no pool, nothing to retire, and this returns
     * at once, which is why every serial section of a test can get the order
     * wrong and never find out.  The destructor does both, in order.
     */
    void join();

    /** The transport underneath: its policy, its peers, its on_error. */
    sys::server& transport();

private:
    // Ahead of the methods that name it: a member declaration is not a
    // complete-class context, so route_for() cannot return a type the class
    // declares further down.
    struct entry {
        std::string method;
        std::string path;

        // One or the other, never both.  A route is registered by whichever
        // overload of route() was called.
        handler run;
        stream_handler stream;
        async_stream_handler async_stream;
    };

    void serve(sys::socketstream& s, const sys::peer& from);

    /** The coroutine an async server runs per connection. */
    sys::task<void> serve_async(sys::server::connection& c,
                                const sys::peer& from);

    /**
     * One request on an async connection.
     *
     * @param served  how many have already been answered on this connection
     * @return        whether the connection may carry another
     */
    sys::task<bool> serve_request_async(sys::server::connection& c,
                                        const sys::peer& from,
                                        std::size_t served);

    /** Shared by both serves: the target as a path, or why it is not one. */
    static bool path_of(const std::string& target, std::string& path,
                        std::string& why);

    /** Shared by both serves: the route table lookup. */
    const entry* route_for(const std::string& method,
                           const std::string& path) const;

    bool m_async = false;

    /**
     * How long an *async* server gives a client to deliver a whole request.
     *
     * Taken from policy::io_timeout, and the two are not quite the same thing
     * -- which is worth saying, because this is the one place the two servers
     * bound a client differently.
     *
     * On the blocking server io_timeout is `SO_RCVTIMEO`: **per operation**,
     * so a client that sends one octet every twenty-nine seconds resets it
     * every time and can hold a connection for as long as it likes.  Here it
     * is a deadline over the *whole* request read, so that client is dropped.
     * That is the slow-loris, and the async server is the one that refuses it.
     *
     * The deadline is cancelled once the request is in hand: a handler that
     * takes a long time to answer is a different question, and bounding it
     * here would turn a slow reply into a dropped connection.
     */
    double m_request_timeout = 30;


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
