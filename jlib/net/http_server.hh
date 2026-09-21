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

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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

    /**
     * What a request for a directory is answered with, in order.
     *
     * Apache's `DirectoryIndex` and nginx's `index`, and the reason a link to
     * a site root works at all: `GET /` names a directory, and without this it
     * is a 404. Apache enables it globally, in `mods-enabled/dir.conf`, which
     * is why a vhost never mentions it and why it is easy to miss when
     * translating one.
     *
     * **Not a directory listing.** When no name here exists the answer is 404,
     * not a generated page -- that is `Options Indexes`, a different feature,
     * and one that publishes what is in a directory to anyone who asks.
     *
     * Empty turns it off, and then a directory is a 404 as it was before.
     */
    std::vector<std::string> index = { "index.html" };
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
 * No HTTP/2 or /3.  No compression (#233).  No `multipart/byteranges`, so a
 * multi-range request gets the whole body (#232).  No `If-Match` or
 * `If-Unmodified-Since`, so a precondition can only succeed -- 412 is
 * unreachable (#231).  No `Vary`, which stays empty until something is
 * negotiated (#233); `Cache-Control` is per `files()` route.
 *
 * Authentication is `protect()` -- Basic and Bearer, per prefix.  No Digest,
 * no sessions, no login form, and **no authorisation**: a verifier says
 * whether credentials are good, never what they may do.
 *
 * Requests are rate-limited per address by `rate_limit()`, off by default.
 * There is still **no per-address connection cap** (#238), so one client can
 * hold every slot `sys::server_policy::max_connections` allows -- the limit
 * above bounds how fast a connection may ask, not how many a client may open.
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

        // What was answered, for the access record.  Body octets only: the
        // status line and fields are not what a Combined log's byte count
        // means, and counting them would make every line disagree with every
        // other server's.
        int         m_status = 0;
        std::size_t m_wrote = 0;

    public:
        /** The status this answered with, or 0 if it has not answered. */
        int status() const { return m_status; }

        /** Body octets written, not counting framing. */
        std::size_t wrote() const { return m_wrote; }

    private:
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
        /**
         * @param conn  the connection this answers on, or null.  Only
         *              `peer_gone()` uses it, and only to ask a question.
         */
        async_responder(sys::async_writer& w, const std::string& server_name,
                        sys::reactor& r, sys::job_queue& pool,
                        const sys::server::connection* conn = 0)
            : m_writer(&w), m_name(server_name), m_reactor(&r), m_pool(&pool),
              m_conn(conn) {}

        /**
         * Has the client gone, asked without writing anything?
         *
         * **For a handler that produces one whole answer after a long time.**
         * A streaming handler learns the client left from a failed write --
         * `live()` below -- which makes that knowledge only as fresh as the
         * last write. A handler that writes nothing for a minute learns
         * nothing for a minute, and spends the minute on a reply nobody will
         * read. jserve's non-streaming path is exactly that shape: a
         * generation holds the model for its whole length whether or not
         * anybody is still waiting.
         *
         * Cheap enough to call between units of work -- a non-blocking peek,
         * no allocation -- and it does not consume, so a pipelined next
         * request is still there for the reader afterwards.
         *
         * **Callable from any thread whose lifetime is bounded by this
         * connection's**, which for a generation thread means one held by a
         * `sys::relay`: it joins in its destructor, so it cannot outlive the
         * frame that owns it, and that frame is destroyed before the
         * descriptor closes. See sys::server::connection::peer_gone().
         *
         * `false` means "not known to have gone" rather than "still there",
         * and over TLS it always means the former -- the descriptor carries
         * ciphertext and cannot be read honestly. A model held a little longer
         * costs time; a reply abandoned on a guess costs the answer.
         */
        bool peer_gone() const {
            return m_conn != 0 && m_conn->peer_gone();
        }

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

        /** The status this answered with, or 0 if it has not answered. */
        int status() const { return m_status; }

        /** Body octets written, not counting framing. */
        std::size_t wrote() const { return m_wrote; }

    private:
        // See the blocking responder: body octets only, so a Combined log's
        // byte count means what it means everywhere else.
        int                m_status = 0;
        std::size_t        m_wrote = 0;

        sys::async_writer* m_writer;
        std::string        m_name;
        bool               m_started = false;
        sys::reactor*      m_reactor = 0;
        sys::job_queue*    m_pool = 0;

        // Only peer_gone() reads it, and only to ask.  A handler gets the
        // question rather than the connection, so the responder stays the one
        // thing that writes.
        const sys::server::connection* m_conn = 0;
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

    /**
     * Several ports at once, each with its own identity.
     *
     * One route table, one set of guards, one limiter, one `on_request` hook
     * and one connection cap across all of them -- **nothing here is keyed by
     * transport**, so a route registered once is served on every port. That is
     * the whole of what this buys over running the program twice.
     *
     * The usual shape is a plaintext port and a TLS one:
     *
     *     std::vector<sys::server::bound> ports;
     *     ports.push_back(sys::server::bound(sys::listener(80, host)));
     *     ports.push_back(sys::server::bound(sys::listener(443, host), tls));
     */
    server(std::vector<sys::server::bound> ports,
           const sys::server::policy& p = sys::server::policy(),
           const options& o = options());

    /** The same, serving on a reactor rather than on threads. */
    server(async_t, std::vector<sys::server::bound> ports,
           const sys::server::policy& p = sys::server::policy(),
           const options& o = options());

    ~server();

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    /**
     * The first listener's port, and whether *that* one is TLS.
     *
     * They describe the same listener, as sys::server's do and for the same
     * reason: read apart they would let a caller dial a plaintext port over
     * TLS while each answer was separately true.
     */
    unsigned short port() const;
    bool tls() const;

    /** Every port, in the order they were given. */
    std::vector<unsigned short> ports() const;

    /**
     * Answer 301 to anything arriving on `from`, pointing at `to` over https.
     *
     * The paradigm port 80 exists for. `Location` is built from the authority
     * the request named, so a redirect serves every site on the listener
     * without being told what they are; the port is omitted when `to` is 443
     * and included otherwise.
     *
     * **It is decided before the guards are**, which is the point rather than
     * an optimisation: `Basic` credentials are base64, and a 401 on a
     * plaintext port asks for a password that anyone on the path can read.
     * Redirecting first means the challenge only ever goes out over TLS.
     *
     * Rate limiting still comes first, so a flood meets the cheap defence
     * before this one.
     */
    void redirect_insecure(unsigned short from, unsigned short to);

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
     * Optionally with a `Cache-Control` value, which is the whole of the
     * freshness story; the two obvious answers are opposites, so they are two
     * registrations:
     *
     *     s.files("/assets/*", "/srv/www/assets",
     *             "max-age=31536000, immutable");   // fingerprinted names
     *     s.files("/*",        "/srv/www", "no-cache");  // revalidate always
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
     * ## How a file is sent
     *
     * A block at a time, so a request costs the block and not the file, and
     * **counted rather than chunked**: `stat` knows the length before anything
     * is read, so the body carries a `Content-Length` and the connection stays
     * reusable without a chunk header per block.
     *
     * A range seeks rather than reading and discarding, which is what streaming
     * bought: `bytes=9500-` on a large file reads from 9500.
     *
     * ## What it is still not
     *
     * No `Cache-Control`, so a client caches by heuristic (#230).  No
     * `multipart/byteranges`, so asking for two ranges gets the whole file
     * (#232).  No compression (#233).  No directory listing and no index.html,
     * which is deliberate -- both are decisions a caller should make out loud.
     *
     * ## Freshness
     *
     * `cache_control` is sent verbatim as `Cache-Control` on every response
     * that describes the file -- the 200, a 206, **and the 304** -- and
     * omitted entirely when empty, which is the default.
     *
     * Per registration rather than per server, because the two obvious answers
     * are opposites: a fingerprinted bundle wants `max-age=31536000,
     * immutable` and an index.html wants `no-cache`.  A caller with both
     * registers `files()` twice.
     *
     * **On the 304 as well** -- RFC 9110 15.4.5 requires a 304 to carry the
     * fields a 200 would have, `Cache-Control` among them, because a client
     * that revalidated and got back a bare 304 would lose the freshness it was
     * told last time and have to ask again immediately.
     *
     * Nothing is invented.  A caller that says nothing gets no `Cache-Control`
     * at all, which is what this did before the parameter existed; a client
     * then caches by heuristic, which is its business rather than this
     * server's guess.  `Vary` is not set either, and matters only once
     * something is negotiated -- see #233.
     *
     * The value is checked against the field grammar **at registration**,
     * where the caller is, rather than failing on the way out of a response.
     *
     * The file is opened once and every later answer comes from that
     * descriptor -- the type, the validators, the length, and the bytes.  The
     * name is resolved afterwards and bound to the descriptor by device and
     * inode, so the file that passed the containment check and the file that
     * is read are the same file (#234).
     *
     * That was not a theoretical window.  Checking the name and then opening
     * it again served a file from outside the root, measurably, on every run
     * of a test that flips a symlink while requests are in flight.
     *
     * And the header above still says this server is not hardened for a public
     * port.  A file server is the feature most likely to make somebody forget
     * that, so: it is still true, and this does not change it.
     *
     * @throws error if `root` cannot be resolved
     */
    void files(const std::string& pattern, const std::string& root,
               const std::string& cache_control = std::string());

    /**
     * What a client sent in `Authorization`, taken apart.
     *
     * `scheme` is as sent and **must be compared case-insensitively** -- it is
     * a token, and RFC 9110 11.1 says so.  `Basic` and `basic` are the same
     * scheme, and a verifier that uses `==` will be bypassed by lowercasing.
     */
    struct credentials {
        std::string scheme;

        /** The token68, when that is what was sent.  Bearer's whole content. */
        std::string token;

        /** Basic only, decoded from `token`; empty for every other scheme. */
        std::string user;
        std::string password;
    };

    /** True if these credentials are acceptable.  Called on the serving thread. */
    typedef std::function<bool(const credentials&)> verifier;

    /**
     * Require credentials on every path matching `pattern`.
     *
     *     s.protect("/admin/*", "Basic realm=\"jlib\"",
     *               [](const server::credentials& c) {
     *                   return c.user == "root" && check(c.password);
     *               });
     *
     * `challenge` is sent verbatim as `WWW-Authenticate` on the 401, and is
     * checked against the grammar **at registration**, where the caller is --
     * the same argument `files()` makes for its `Cache-Control`.  A 401 whose
     * challenge a client cannot parse is a refusal that does not say what to
     * do instead, which is the whole reason the field exists.
     *
     * ## Checked before routing, deliberately
     *
     * A protected path that does not exist answers 401, not 404.  Routing
     * first would make the 404 a directory listing for anyone patient enough
     * to ask -- the status alone says which paths are real.  The cost is that
     * a typo under a protected prefix looks like a credentials problem.
     *
     * Where several patterns match, the most specific wins, by the same count
     * of literal segments `route()` uses.  So `/admin/*` can be Basic while
     * `/admin/api/*` is Bearer.
     *
     * ## What this is not
     *
     * - **Not authorisation.** The verifier says whether the credentials are
     *   good, not what they may do.  A verifier that wants per-path rules gets
     *   no path, on purpose: that is a second decision and it belongs to a
     *   handler that knows what it is guarding.
     * - **No Digest** (RFC 7616): effectively dead, and its nonce bookkeeping
     *   is server-side state for a scheme nothing uses over TLS.
     * - **No session, no cookie, no login form.**  Those are an application's,
     *   and every one of them is built on top of this rather than beside it.
     *
     * ## Basic is cleartext
     *
     * `Basic` is base64, which is not encryption: anyone who can read the
     * connection can read the password. It is only ever safe over TLS, and
     * this server will not stop you using it without.  Bearer is no better --
     * a token in a header is a password with a different name.
     *
     * @throws error if the pattern is not a usable route pattern, if
     *         `challenge` is empty, or if it is not a usable challenge
     */
    void protect(const std::string& pattern, const std::string& challenge,
                 verifier v);

    /**
     * Everything registered through this answers only for one site.
     *
     *     s.site("a.example").files("/*", "/srv/a");
     *     s.site("b.example").files("/*", "/srv/b");
     *     s.files("/*", "/srv/default");       // any other name
     *
     * ## How a request finds its site
     *
     * By the authority it named -- the absolute-form target's, or `Host` --
     * which #284 made unambiguous first, because routing on a field with three
     * possible answers is not routing. Compared **exactly**, after that
     * normalisation: lowercased, port removed.
     *
     * No wildcards. `*.example.com` is a thing people want and a second way to
     * pick the wrong site, and the safe version of it needs rules about how
     * many labels match and which of two patterns wins. Exact names first;
     * whoever needs patterns can have them with a test that says what they
     * mean.
     *
     * ## And what happens to a name nothing claims
     *
     * **A site-qualified route wins; otherwise the unqualified ones answer.**
     * So registrations made on the server itself are the default site, and a
     * request for a name no `site()` claimed lands there rather than nowhere.
     *
     * That is the safe direction, and it is worth saying which way round it
     * is: the *fallback* is chosen by the server's configuration, never by the
     * request. A client cannot name its way into a site that was not
     * registered for it -- the worst it can do is fail to name one and get the
     * default, which is what a server with one site has always done.
     *
     * Per request rather than per container, unlike nginx: a path with no
     * site-qualified route still reaches an unqualified one. One `site()` call
     * therefore does not hide the rest of the table from that name, which is a
     * cliff worth not having.
     *
     * ## Guards too
     *
     * `protect()` on a site guards only that site. A guard registered on the
     * server guards every site that has no guard of its own for that path --
     * same rule, because a guard that stopped applying because somebody added
     * a site would be the worst possible way to lose one.
     */
    class site {
    public:
        site(server& s, const std::string& name) : m_s(&s), m_name(name) {}

        void route(const std::string& method, const std::string& path,
                   handler h);
        void route(const std::string& method, const std::string& path,
                   stream_handler h);
        void route(const std::string& method, const std::string& path,
                   async_stream_handler h);
        void route(const std::string& method, const std::string& path,
                   param_handler h);
        void route(const std::string& method, const std::string& path,
                   param_stream_handler h);
        void route(const std::string& method, const std::string& path,
                   async_param_stream_handler h);

        void files(const std::string& pattern, const std::string& root,
                   const std::string& cache_control = std::string());

        /**
         * Guard a pattern **on this site only**.
         *
         * A guard carrying a host is skipped for every other host, which is
         * what scoping means and is the point of it. The trap is that a
         * *route* carrying no host still matches every host -- so this:
         *
         *     s.route("GET", "/secret", serve_it);          // no site
         *     s.site_of("private.example")
         *      .protect("/secret", challenge, verify);      // this site
         *
         * leaves `/secret` guarded under `private.example` and **open under
         * any other Host the client cares to send**. Measured: 401 for the
         * one, 200 for the other.
         *
         * Neither half is wrong on its own and the server cannot tell which
         * you meant, so this is a note rather than a refusal. Scope the route
         * as well, or guard from the server rather than the site -- which is
         * what jhttpd does, and why its config has no way to write the shape
         * above.
         */
        void protect(const std::string& pattern, const std::string& challenge,
                     verifier v);

        const std::string& name() const { return m_name; }

    private:
        server*     m_s;
        std::string m_name;
    };

    /**
     * A handle for registering against one name.  Cheap; make it inline.
     *
     * @throws error if `name` is empty -- which would mean "any site", and
     *         the server's own methods already say that more plainly
     */
    site site_of(const std::string& name);

    /**
     * Refuse more than `per_second` requests from one address, 429.
     *
     *     s.rate_limit(10, 20);   // ten a second, twenty may arrive at once
     *
     * A token bucket per address: `burst` tokens to start, one spent per
     * request, refilled at `per_second`.  So a client may arrive all at once
     * up to `burst` and then settles to `per_second`, which is the behaviour
     * anything bursty -- a page and its dozen assets -- needs to not be broken
     * by a limit meant for an attacker.
     *
     * Zero `per_second` turns it off, and off is the default: a limit is a
     * policy about who is calling, and a library that guessed one would be
     * wrong for a loopback receiver and for a public port in opposite
     * directions.
     *
     * The refusal is a **429 with `Retry-After`**, in seconds, rounded up --
     * the counterpart of 401's `WWW-Authenticate` and 405's `Allow`: a
     * refusal that says what to do instead. The connection stays open, so a
     * client that backs off does not pay for a new one.
     *
     * ## Per request, and after the head is read
     *
     * Checked once the request head has been parsed, so a refused request has
     * already cost a parse. Refusing before that would mean refusing per
     * *connection*, which is a different limit -- and the thing this exists to
     * stop, a thousand password guesses, fits comfortably down one connection.
     *
     * It runs **before** `protect()`, so a 429 can be reached without
     * credentials. A rate limit that only applied to requests which got past
     * authentication would be no protection for the thing most worth
     * protecting.
     *
     * ## The address is the peer, and only the peer
     *
     * `X-Forwarded-For` is **not** read. It is client-supplied, so believing
     * it unconditionally lets anybody be any address, which is worse than no
     * limit at all -- it is a limit an attacker can aim at somebody else.
     *
     * The consequence is the one to know before deploying: **behind a proxy,
     * every client is one address**, so this limits the proxy. A list of
     * proxies whose forwarded headers are believed -- nginx's
     * `set_real_ip_from` -- is what fixes it, and it belongs with jhttpd's
     * configuration rather than here (#239).
     *
     * Loopback is not exempt. An exemption would be untestable, since every
     * test in this tree connects over loopback.
     *
     * @param per_second requests per second per address; 0 turns it off
     * @param burst      how many may arrive at once; clamped to at least 1
     */
    void rate_limit(double per_second, double burst);

    /**
     * A token bucket per address.
     *
     * **Public so it can be tested at all.**  Per-address separation is the
     * whole of what this does, and it cannot be reached through the server:
     * every client in this tree connects over loopback, and both platforms
     * hand the server 127.0.0.1 as the peer whichever loopback address is
     * dialled -- measured, not assumed.  So the algorithm is tested here
     * directly, with addresses supplied as strings, and the server tests cover
     * the wiring.  It is a small enough thing to be useful on its own.
     *
     * Locked, because the two servers reach it from different threads: a
     * blocking server from whichever worker took the connection, an async one
     * from the reactor thread. One mutex over a small map.
     *
     * **The critical section is not always arithmetic**, and this comment
     * used to say it was. `allow()` calls `sweep()` inside the lock when the
     * table is over `max_tracked`, which walks every bucket and partitions
     * them. Measured at 4096 tracked addresses, on an idle machine:
     *
     *     median call            0.12us
     *     p99                    0.25us
     *     a call that sweeps      ~80us
     *
     * Amortised that is nothing -- one sweep per ~1024 new addresses, so
     * ~0.1us per request -- but on the reactor the tail is what shows, and a
     * comment claiming "the arithmetic and nothing else" is what stops the
     * next reader from noticing. #276 assessed this at "tens of nanoseconds"
     * from the same mistake.
     *
     * The trigger is attacker-reachable: a bucket per source address, and an
     * IPv6 allocation makes source addresses free. `max_tracked` is what
     * keeps that bounded, and is the reason the cost has a ceiling at all.
     */
    class limiter {
    public:
        void configure(double per_second, double burst);

        /**
         * How many addresses are currently remembered.
         *
         * Public because it is the only externally visible consequence of the
         * sweep, and a table that grows without bound is the failure mode this
         * component has.  An operator watching one number should watch this.
         */
        std::size_t tracked() const;

        /**
         * Spend a token for `who`, or say how long until there is one.
         *
         * @return true if the request may proceed; otherwise `retry_after` is
         *         set to whole seconds, at least 1
         */
        bool allow(const std::string& who, long& retry_after);

    private:
        struct bucket {
            double                                tokens = 0;
            std::chrono::steady_clock::time_point when;
        };

        /**
         * Drop every bucket that is full, and then, if that was not enough,
         * the fullest of what is left.
         *
         * The first pass is **exact, not an approximation**: a bucket refilled
         * to `burst` is indistinguishable from an address never seen before --
         * both start the next request with a full bucket -- so erasing it
         * changes no answer this will ever give.
         *
         * The first pass alone does not bound anything, which is the part that
         * had to be measured rather than assumed. It frees a bucket only once
         * that bucket has had time to refill, so when addresses arrive faster
         * than a token is worth, **nothing is ever full and nothing is ever
         * freed**. 50,000 distinct addresses at a slow rate kept 50,000
         * buckets, and every request past `sweep_at` walked all of them. A
         * rate limiter that an attacker turns into unbounded memory and an
         * O(n) scan per request by varying a source address -- trivial with a
         * v6 allocation -- is worse than no rate limiter.
         *
         * So there is a hard bound. Over `max_tracked`, the **fullest**
         * buckets go first: evicting a bucket grants its address a fresh
         * allowance, and the fullest is the one closest to having that anyway,
         * so it is the least that can be given away. Under address flooding
         * the limit degrades toward permissive rather than growing without
         * bound, and that is a choice -- the other way, refusing to track a
         * new address, denies service to everybody the attacker is not.
         *
         * Runs only when the table is over `max_tracked`, and clears it down
         * to three quarters of that rather than to exactly the bound. The
         * hysteresis is what makes the cost amortised: without it, every
         * request past the bound would pay for a full scan and a sort, which
         * is a worse version of the problem being fixed.
         */
        void sweep(std::chrono::steady_clock::time_point now);

        /**
         * The hard bound on the table, whatever the first pass freed.
         *
         * Not measured -- the kind of constant #235 is about -- and it is a
         * policy rather than a tuning knob: it is how many addresses an
         * attacker must flood before the limit begins to give way, and how
         * much memory that costs.
         */
        static const std::size_t max_tracked = 4096;

        mutable std::mutex             m_lock;
        double                         m_rate = 0;
        double                         m_burst = 0;
        std::map<std::string, bucket>  m_buckets;
    };


    /**
     * One answered request, as a log needs it.
     *
     * **The fields are chosen for the Combined Log Format** and nothing else:
     * that format is what existing tools read, and a server that invents its
     * own gives an operator a log nothing analyses. What is not here is not
     * omitted by accident -- it is not in the format.
     *
     * Every string except `peer`, `status` and `bytes` is **client-supplied**.
     * See the note on `on_request()` about what that means for whoever writes
     * them down.
     */
    struct access {
        /** Numeric, from the accepting side.  Never a name. */
        std::string peer;

        /**
         * The peer's ephemeral port.
         *
         * Not in the Combined format and never logged by it. It is here
         * because an *error* line identifies a client as `ip:port`, the way
         * Apache's does, and a refusal has to name the connection it refused
         * rather than merely the address it came from.
         */
        unsigned short peer_port = 0;

        /**
         * Who `protect()` let through, or empty.  Combined's third field.
         *
         * **The one field that did not pass the header grammar.** Every other
         * string here was read as a header value, and a raw control character
         * in one of those is refused with a 400 before any of this is
         * reached. `Authorization: Basic <base64>` is valid to the grammar
         * whatever the base64 decodes to, so this arrives carrying anything a
         * client chose -- CR and LF included.
         *
         * That is not a defect and it is not filtered here: what a credential
         * contains is the verifier's business, and a `protect()` callback
         * that accepts an odd username has said it will. But a consumer that
         * writes this somewhere line-oriented must escape it, and is the only
         * one of these fields for which "the grammar already checked" is
         * false.
         *
         * `jhttpd::escaped()` does, and `app_jhttpd_test` has the case.
         */
        std::string user;

        /**
         * Which site the request named, lowercased and without a port.
         *
         * From the **request target** when it is in absolute form and from
         * `Host` otherwise, which is RFC 9112 3.2.2's order rather than a
         * preference. Empty only for HTTP/1.0 with no Host, which is the one
         * case the RFC still allows.
         *
         * Not part of the Combined format -- Apache calls it `%v` and puts it
         * in a different one -- so a log that wants it has to say so. It is
         * here because a server that answers for more than one name has
         * nothing else to tell them apart by.
         */
        std::string host;

        /** As sent, undecoded: the log records what arrived, not what it meant. */
        std::string method;
        std::string target;
        std::string version;

        std::string referer;
        std::string user_agent;

        /** 0 when the connection died before anything was answered. */
        int status = 0;

        /** Body octets, not counting the head or chunk framing. */
        std::size_t bytes = 0;

        /**
         * Which listener answered, and whether it speaks TLS.
         *
         * Apache had no need of these because each `<VirtualHost *:80>` wrote
         * its own log. One process serving both ports is what created the
         * question, and `host` alone cannot answer it -- the same name is
         * served on both.
         *
         * Apache spells the pair `%p` and puts it, with `%v`, in the
         * `vhost_combined` format rather than in Combined.
         */
        unsigned short local_port = 0;
        bool           secure = false;

        /**
         * Why this request was refused, or empty if it was not.
         *
         * **Set only for a refusal the server diagnosed**, not for an ordinary
         * 404: a missing file is the normal traffic of the web and logging it
         * as an error is how an error log becomes unreadable. A target that
         * could not be parsed, a host that could not be resolved to a site, a
         * guard that said no -- those are the lines Apache writes and this
         * server did not.
         *
         * Client-supplied, like every other string here: it quotes the value
         * that caused the refusal, which is a value the client chose.
         */
        std::string reason;
    };

    /**
     * Called once per answered request, on the thread that answered it.
     *
     * This server does not log. It cannot: a library does not know where its
     * output goes, whether there is a file, whether two threads are writing to
     * it, or what should happen when the disk fills. It knows what happened,
     * which is this.
     *
     * ## Whoever writes these down owns an injection problem
     *
     * `method`, `target`, `referer` and `user_agent` are chosen by the client,
     * and a Combined log line is newline-delimited and quote-delimited. A
     * target containing a CR, an LF or a `"` forges log entries -- and a log
     * an attacker can write is worse than no log, because it is believed.
     *
     * They are handed over **raw and undecoded on purpose**. A record that
     * arrived escaped could not be used for anything but a log, and the
     * escaping a log wants is not the escaping anything else wants. The
     * server's own defences do not help here either: `path_of` refuses control
     * characters in the *path*, and none of these four fields is the path.
     *
     * `jhttpd` escapes them on the way to the file. Anything else that writes
     * one down has to do the same.
     *
     * ## Cost
     *
     * Called with the response already sent, so a slow handler here delays the
     * next request on that connection rather than this one's answer. On the
     * async server that is the reactor thread; a hook that writes to a file is
     * doing I/O on it, which is the reason jhttpd's writer is what it is.
     */
    void on_request(std::function<void(const access&)> h);

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

        // Which site this answers for, folded; empty means any of them.
        // See site().
        std::string host;

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
     * One protected prefix.
     *
     * The pattern lives in an `entry` so that `compile_route` and `matches`
     * are the same code routes use -- a guard whose patterns behaved subtly
     * differently from the routes they guard is the bug this avoids.  Its
     * handler fields are all empty and never read.
     */
    struct guard {
        // Same rule as entry::host, and for the same reason.
        std::string host;

        entry       where;
        std::string challenge;
        verifier    check;
    };

    /** The most specific guard matching `path`, or null. */
    const guard* guard_for(const std::string& path,
                           const std::string& site) const;

    /** Build and deliver one access record, if anybody asked for them. */
    void note(const util::http::Request& q, const sys::peer& from,
              const std::string& user, const std::string& host, int status,
              std::size_t bytes, const std::string& reason) const;

    /**
     * Decide the 429, if there is one.
     *
     * @return true if the request may proceed; otherwise `r` is the answer.
     */
    bool within_rate(const sys::peer& from, response& r);

    /**
     * Decide the 401, if there is one.
     *
     * @return true if the request may proceed; otherwise `r` is the answer.
     */
    bool allowed_through(const util::http::Request& q, const std::string& path,
                         const std::string& site, response& r,
                         std::string& who) const;

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

    /**
     * Which authority this request names, normalised -- or why to refuse it.
     *
     * See the definition: RFC 9112 3.2 makes a missing or repeated Host a 400
     * on HTTP/1.1, and 3.2.2 makes an absolute-form target's authority win
     * over the field.
     */
    static bool authority_of(const util::http::Request& q, std::string& into,
                             std::string& why);

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
                           const std::string& site, params& into,
                           std::vector<std::string>& allowed) const;

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
    std::vector<guard> m_guards;
    std::function<void(const access&)> m_on_request;
    limiter m_limits;
    handler m_otherwise;
    /** Where an insecure request should have gone, or "" if it should stay. */
    std::string redirected(const sys::peer& from, const std::string& authority,
                           const std::string& target) const;

    std::unique_ptr<sys::server> m_transport;
    bool m_tls = false;

    /**
     * Plaintext port -> the https port it is answered with.
     *
     * A map rather than a flag because the marker is per listener: a server
     * may have a port that redirects and another that genuinely serves.
     * Empty for nearly every server, and looked at once per request.
     */
    std::map<unsigned short, unsigned short> m_redirect;
};

}
}
}

#endif // JLIB_NET_HTTP_SERVER_HH
