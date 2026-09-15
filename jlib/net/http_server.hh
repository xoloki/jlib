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
 * ## It was a harness; it is a small server now
 *
 * This header said "as narrow as the client" for most of its life, and that
 * stopped being true one branch at a time.  What it does now:
 *
 *   - **keep-alive** on the asynchronous server, with three separate bounds --
 *     see server_options.  The blocking one still answers once and closes, and
 *     should: an idle persistent connection is a suspended coroutine here and
 *     would be a held thread there.
 *   - **chunked output**, so a streamed body has an end a client can find.
 *     It used to be delimited by the close, which meant a handler that died
 *     halfway produced the same octets as one that finished.
 *   - **route patterns** -- `/users/{id}`, `/static/*` -- with the most
 *     specific match winning, and 405 with `Allow` where a path exists for
 *     another method.
 *   - **HEAD**, answered by the GET route with the body suppressed and the
 *     Content-Length a GET would have sent.
 *   - **conditional requests**: `If-None-Match` and `If-Modified-Since` become
 *     304, with the HTTP-date read against RFC 9110's own grammar.
 *   - **byte ranges** for any body, and `files()` for serving a directory --
 *     which seeks rather than reading, so a range costs the range.
 *
 * A buffered response is still accumulated whole, which is what lets a handler
 * that throws be answered with a 500 anyway.  A streaming one gives that up
 * knowingly; `files()` keeps it regardless by deciding everything -- 404, 304,
 * 416 -- before a byte goes out.
 *
 * ## What it is still not
 *
 * No HTTP/2 or /3.  No compression.  No `multipart/byteranges`, so a
 * multi-range request gets the whole body.  No `If-Match` or
 * `If-Unmodified-Since`, so a precondition can only succeed -- 412 is
 * unreachable.  No `Cache-Control`, no authentication, no rate limiting, no
 * per-address anything.
 *
 * **It is not hardened for a public port** -- see the note on sys::server --
 * and that sentence has more to protect now than when it was written.  It
 * began as a thing to receive an OAuth2 redirect on loopback and to be a test
 * harness; a server that routes, caches and serves a directory is a much
 * easier thing to point at the internet by mistake.
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
         * The same octets a GET would have produced, without the body.
         *
         * Which is what a HEAD is owed -- RFC 9110 9.3.2 says the headers
         * SHOULD be those of the GET, and that includes the Content-Length
         * the body *would* have had.  Computing it any other way would make a
         * HEAD a different question from the GET it is supposed to preview.
         */
        std::string without_body(const std::string& server_name,
                                 bool persist = false) const;

        /**
         * The head alone, with no Content-Length.
         *
         * For a response whose length is not known when it starts; the body
         * follows from responder::write and is delimited by the close.
         */
        /**
         * The head alone, for a body whose length is not known yet.
         *
         * @param chunked  frame what follows with Transfer-Encoding: chunked
         * @param persist  whether the connection carries another message after
         *                 this one.  Only possible when chunked: a
         *                 close-delimited body ends at the close, so there is
         *                 no boundary for a next message to begin at.
         */
        std::string head(const std::string& server_name, bool chunked = false,
                         bool persist = false) const;

    private:
        std::string serialise(const std::string& server_name,
                              bool with_length, bool persist,
                              bool chunked = false) const;

        int m_status = 200;
        std::string m_reason;
        http::fields m_fields;
        std::string m_body;
    };

    /**
     * What a route pattern captured out of the target.
     *
     * A pattern is matched segment by segment against the path, and two kinds
     * of segment capture rather than compare:
     *
     *     /users/{id}        {id} matches one segment, by that name
     *     /static/*          * matches the rest, whatever its depth
     *
     * **A capture is already decoded**, because the path is: path_of() runs
     * util::uri::decode over the target before anything is matched, so `%65`
     * arrives as `e`.
     *
     * It therefore cannot contain a separator.  `%2F` and `%5C` are refused
     * outright, with a 400, before routing is reached -- decoding one would
     * change how many segments the path has, which is the shape of a traversal
     * bug -- and a literal `/` ends the segment.  So a parameter is one
     * segment's worth of decoded text and nothing more, which is the property
     * a handler building a filename off one is relying on whether it knows it
     * or not.
     */
    class params {
    public:
        bool has(const std::string& name) const;

        /** The capture, or empty if the pattern had no such name. */
        std::string get(const std::string& name) const;

        /**
         * What `*` matched, with no leading slash, or empty for a pattern
         * without one.
         *
         * Note that it may contain slashes, and **may contain `..`**: this is
         * the raw remainder, not a file path, and a handler turning one into a
         * path is the thing that has to say so.
         */
        const std::string& rest() const { return m_rest; }

        std::size_t size() const { return m_named.size(); }

    private:
        friend class server;

        std::vector<std::pair<std::string, std::string> > m_named;
        std::string m_rest;
    };

    typedef std::function<void(const Request&, response&)> handler;

    /** The same, for a route whose pattern captures something. */
    typedef std::function<void(const Request&, const params&, response&)>
        param_handler;

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
     * No `Content-Length`, because the length is not known.  What delimits the
     * body instead is `Transfer-Encoding: chunked` for an HTTP/1.1 client, and
     * the close for anything older.
     *
     * **It used to be the close in both cases**, and the reason written here
     * was that "chunked would buy nothing against a server that closes
     * anyway".  That stopped being true when the async server learned
     * keep-alive: it does not close anyway, and a close-delimited body was
     * then the only thing left that forced it to.
     *
     * Chunked earns its keep a second time even when the connection *is* about
     * to close, and this is the half that was wrong all along: a
     * close-delimited body gives the client no way to tell a complete response
     * from one whose server died halfway through.  A terminating chunk does.
     * So a handler that fails mid-stream now produces a body that is visibly
     * unfinished rather than one that merely stops.
     */
    class responder {
    public:
        responder(sys::socketstream& s, const std::string& server_name)
            : m_s(&s), m_name(server_name) {}

        /**
         * How the body after begin() is framed.
         *
         * Decided by the server once it has read the request, because that is
         * what decides it: chunked needs a client that speaks HTTP/1.1.  Must
         * precede begin(), which is where it takes effect.
         */
        void framing(bool chunked);

        bool chunked() const { return m_chunked; }

        /**
         * Write the head and nothing after it.
         *
         * Set by the server for a HEAD request, before the handler runs,
         * because by the time one has called begin() the head has gone.  The
         * handler is not told and does not need to be: it produces what it
         * would have produced and write() drops it, which is what keeps a
         * HEAD's headers identical to the GET's.
         */
        void suppress_body();

        /** Send a complete response.  Exactly what a buffered handler does. */
        void send(const response& r);

        /**
         * Send the head now and commit the status.
         *
         * The response's body is ignored -- what follows comes from write().
         * `Content-Length` is omitted whatever the handler set, because a
         * length that later disagrees with the body is a framing bug and this
         * is the one place the server can be sure it would.  What delimits the
         * body instead is decided by framing(), above.
         */
        void begin(const response& head);

        /**
         * One piece of the body.
         *
         * An empty piece is **ignored rather than written**, because a
         * zero-length chunk is how a chunked body says it has ended: a handler
         * that wrote one would terminate its own response early and the client
         * would believe it had the whole thing.
         */
        void write(const std::string& piece);

        /**
         * Finish the body.
         *
         * Writes the terminating chunk, and is **only reached when the handler
         * returned normally.**  A handler that fails partway through leaves
         * the body unterminated and the connection closed, so the client sees
         * a truncated message rather than a complete one -- which is why a
         * streaming response wants chunked framing even on a connection that
         * is about to close anyway.
         */
        void end();

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
        bool m_chunked = false;
        bool m_ended = false;
        bool m_no_body = false;
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

    /** The same, for a route whose pattern captures something. */
    typedef std::function<void(const Request&, const params&, responder&)>
        param_stream_handler;

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
        async_responder(sys::async_writer& w, const std::string& server_name,
                        sys::reactor& r, sys::job_queue& pool)
            : m_writer(&w), m_name(server_name), m_reactor(&r), m_pool(&pool) {}

        /**
         * The reactor this connection is on, and the pool beside it.
         *
         * A streaming handler is the one kind that needs them.  A buffered
         * handler is hopped to a worker by the server and never sees either;
         * a streaming one does its own writing, so it stays where the writer
         * is -- and anything it does that would block has to be got off this
         * thread by the handler itself.
         *
         * `co_await sys::on_pool(out.pool())` for work measured in
         * microseconds.  **For work measured in seconds -- an inference, a
         * subprocess, anything external -- neither of these is the answer**:
         * the pool's threads are there to handle requests, and one held for a
         * minute is one not handling any.  Give that its own thread and let it
         * report back through a descriptor this can wait on.
         */
        sys::reactor& reactor() { return *m_reactor; }

        sys::job_queue& pool() { return *m_pool; }

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
         * Content-Length is omitted whatever the handler set; what delimits
         * the body is decided by framing(), above.  See the responder class
         * comment for why that is chunked rather than the close.
         */
        sys::task<void> begin(const response& head);

        /**
         * How the body after begin() is framed.
         *
         * Decided by the server once it has read the request, because that is
         * what decides it: chunked needs a client that speaks HTTP/1.1.  Must
         * precede begin(), which is where it takes effect.
         */
        void framing(bool chunked, bool persist);

        bool chunked() const { return m_chunked; }

        /** Write the head and nothing after it.  See responder. */
        void suppress_body();

        /**
         * Whether the connection can carry another request after this body.
         *
         * Read back rather than assumed, because framing() is where the two
         * questions are reconciled: a caller may want to persist and still not
         * get to, if what it is about to send is delimited by the close.
         */
        bool persist() const { return m_persist; }

        /** Whether begin() was called, as opposed to send().  See end(). */
        bool begun() const { return m_begun; }

        /**
         * One piece of the body.
         *
         * An empty piece is **ignored rather than written**, because a
         * zero-length chunk is how a chunked body says it has ended: a handler
         * that wrote one would terminate its own response early and the client
         * would believe it had the whole thing.
         */
        sys::task<void> write(const std::string& piece);

        /**
         * Finish the body.
         *
         * Writes the terminating chunk, and is **only reached when the handler
         * returned normally.**  A handler that fails partway through leaves
         * the body unterminated and the connection closed, so the client sees
         * a truncated message rather than a complete one -- which is why a
         * streaming response wants chunked framing even on a connection that
         * is about to close anyway.
         */
        sys::task<void> end();

        bool started() const { return m_started; }

    private:
        sys::async_writer* m_writer;
        std::string        m_name;
        bool               m_started = false;
        sys::reactor*      m_reactor = 0;
        sys::job_queue*    m_pool = 0;
        bool               m_chunked = false;

        // What framing() was told the *client* would accept, before the
        // question of how this particular response is framed.  A body with a
        // Content-Length can be followed by another request; one delimited by
        // the close cannot -- so the two paths out of here answer differently
        // from the same wish, and m_persist is the answer rather than the wish.
        bool               m_want_persist = false;
        bool               m_persist = false;
        bool               m_begun   = false;
        bool               m_ended   = false;
        bool               m_no_body = false;
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

    /** The same, for a route whose pattern captures something. */
    typedef std::function<sys::task<void>(const Request&, const params&,
                                          async_responder&)>
        async_param_stream_handler;

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
     * Route a method and a path pattern.
     *
     * The path is the target's path component with the query removed.  A
     * pattern is matched segment by segment, and a segment is one of three
     * things:
     *
     *     /hello             a literal, compared
     *     /users/{id}        one segment, captured by that name
     *     /static/*          the rest, however deep, captured as params::rest
     *
     * `*` may only be the last segment, a parameter must be named, and a brace
     * in a literal is a typo -- all three are **refused at registration**,
     * where the caller is, rather than at match time where the only symptom
     * would be a route that never fires.
     *
     * **The most specific match wins**, counted in literal segments, so
     * `/static/index.html` beats `/static/*` however they were registered.
     * A tie goes to whichever was added first, which is what the old
     * exact-match scan did for the only case it had.
     *
     * Empty segments are dropped, so `/a//b/` and `/a/b` are one route.  RFC
     * 3986 makes them different resources; this makes them the same, because
     * the alternative is a table where a trailing slash silently 404s.
     *
     * A pattern without a `*` must account for **every** segment: `/exact`
     * does not answer `/exact/extra`.  Without that every route would quietly
     * be a prefix.
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

    /**
     * The three above again, for a pattern that captures.
     *
     * Registering `/users/{id}` or `/static/*` with a handler that takes no
     * params is legal and loses the captures, which is occasionally what a
     * caller wants -- a prefix route that serves one thing regardless of the
     * rest.  These are for when it is not.
     */
    void route(const std::string& method, const std::string& path,
               param_handler h);

    void route(const std::string& method, const std::string& path,
               param_stream_handler h);

    void route(const std::string& method, const std::string& path,
               async_param_stream_handler h);

    /**
     * Serve files under `root` on a wildcard pattern.
     *
     *     s.files("/static/*", "/srv/www");
     *
     * Registers a GET route, which HEAD then answers too.  The file's
     * modification time and size become `Last-Modified` and a **weak** `ETag`,
     * so `If-None-Match` and `If-Modified-Since` work without the caller doing
     * anything -- this is the first thing in the server that *produces* the
     * validators the conditional code could until now only read.
     *
     * ## What it refuses, and how
     *
     * Every refusal is a **404**, including the ones that are really "you may
     * not": a 403 would confirm that something is there, and the whole point
     * of the checks below is to say nothing about what is outside the root.
     *
     * Containment is decided by `realpath`, not by inspecting the path.  A
     * string check catches `..` and misses a symlink; resolving first catches
     * both, because what comes back is where the kernel would actually go.
     * The root is resolved once, here, so a caller passing a path that does
     * not exist finds out at registration.
     *
     * Only regular files.  A directory is a 404: there is no listing and no
     * index.html, because both are decisions a caller should make out loud.
     *
     * ## What it is still not
     *
     * The file is read whole into memory, like every other buffered response.
     * That is fine for the pages and scripts this is for and wrong for a large
     * download; chunked output exists now, so a streaming variant is possible
     * and is not here.
     *
     * And the header above still says this server is not hardened for a public
     * port.  A file server is the feature most likely to make somebody forget
     * that, so: it is still true, and this does not change it.
     *
     * @throws error if `root` cannot be resolved
     */
    void files(const std::string& pattern, const std::string& root);

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
    /** One segment of a route pattern. */
    struct segment {
        enum kind { literal, named, rest };

        kind        what = literal;
        std::string text;   // the literal, or the parameter's name
    };

    struct entry {
        std::string method;
        std::string path;

        // The pattern, parsed once at registration.  Empty `segs` with a
        // non-empty path cannot happen: "/" parses to no segments and matches
        // only itself.
        std::vector<segment> segs;
        bool wild = false;          // the pattern ends in *
        int literals = 0;           // how specific it is; see route_for

        // One of these, never more.  A route is registered by whichever
        // overload of route() was called.
        handler run;
        stream_handler stream;
        async_stream_handler async_stream;
        param_handler param_run;
        param_stream_handler param_stream;
        async_param_stream_handler async_param_stream;
    };

    /** Split a pattern or a path into segments, ignoring empty ones. */
    static std::vector<std::string> split_path(const std::string& path);

    /** Parse a pattern into `e`, or throw if it cannot be one. */
    static void compile_route(entry& e);

    /**
     * Does this entry's pattern match `parts`?  If so, fill `into`.
     *
     * Method is not considered: route_for asks about the path first so that a
     * path known for another method can be answered 405 rather than 404.
     */
    static bool matches(const entry& e, const std::vector<std::string>& parts,
                        params& into);

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
    /**
     * The route for this request, and what its pattern captured.
     *
     * **The most specific match wins**, where specific means the most literal
     * segments -- so `/static/index.html` beats `/static/*`, and `/users/me`
     * beats `/users/{id}`.  Ties go to whichever was registered first, which
     * is the old behaviour and the only part of this a caller could already
     * depend on.
     *
     * @param allowed  the methods that *would* have matched this path.  That
     *                 is the difference between 405 and 404, and it is a
     *                 question about routes rather than status codes, which is
     *                 why it is answered here.
     *
     *                 **Meaningful only when this returns null.**  When a
     *                 route did match it may hold whatever was collected on
     *                 the way past other routes, and a caller has no business
     *                 reading it.
     */
    const entry* route_for(const std::string& method, const std::string& path,
                           params& into, std::vector<std::string>& allowed) const;

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
