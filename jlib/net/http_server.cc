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

#include <jlib/net/http_server.hh>

#include <jlib/sys/await.hh>

#include <exception>

#include <jlib/util/URL.hh>
#include <jlib/util/util.hh>

#include <ctime>
#include <sstream>

namespace jlib {
namespace net {
namespace http {

/** "jlib/" and the release.  See net::http::default_user_agent. */
std::string default_server_name() { return "jlib/" JLIB_RELEASE_STRING; }

namespace {

    /**
     * One chunk, as RFC 9112 7.1 spells it: the size in hex, CRLF, the octets,
     * CRLF.
     *
     * Lower-case hex and no chunk extensions -- both are what the grammar in
     * rfc9112.hh accepts, and this server's own reader parses what this writes
     * with `chunk-size` out of that same grammar.  Writing something its
     * reader would refuse is the failure mode worth ruling out by
     * construction.
     */
    std::string chunk(const std::string& piece) {
        std::ostringstream o;

        o << std::hex << piece.size() << "\r\n" << piece << "\r\n";

        return o.str();
    }

    /** The terminating zero-length chunk, and an empty trailer section. */
    std::string last_chunk() { return "0\r\n\r\n"; }

}

namespace {

    /** RFC 9110 5.6.7's IMF-fixdate, which is fixed-format and never localised. */
    std::string http_date() {
        static const char* const DAY[] = { "Sun", "Mon", "Tue", "Wed", "Thu",
                                           "Fri", "Sat" };
        static const char* const MONTH[] = { "Jan", "Feb", "Mar", "Apr", "May",
                                             "Jun", "Jul", "Aug", "Sep", "Oct",
                                             "Nov", "Dec" };

        const std::time_t now = std::time(0);

        std::tm tm;

        // Spelled out rather than strftime, because strftime's %a and %b are
        // whatever the locale says and RFC 9110 requires these exact names.
        if(::gmtime_r(&now, &tm) == 0) return std::string();

        char buf[64];

        std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                      DAY[tm.tm_wday % 7], tm.tm_mday, MONTH[tm.tm_mon % 12],
                      tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);

        return buf;
    }

    const char* reason_for(int status) {
        switch(status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default:  return "";
        }
    }

}

// ------------------------------------------------------------------ response

server::response& server::response::status(int code, const std::string& reason) {
    m_status = code;
    m_reason = reason.empty() ? reason_for(code) : reason;

    return *this;
}

server::response& server::response::field(std::string name, std::string value) {
    m_fields.add(std::move(name), std::move(value));

    return *this;
}

server::response& server::response::type(const std::string& content_type) {
    return field("Content-Type", content_type);
}

server::response& server::response::body(std::string body) {
    m_body = std::move(body);

    return *this;
}

std::string server::response::head(const std::string& server_name,
                                   bool chunked, bool persist) const
{
    return serialise(server_name, false, persist, chunked);
}

std::string server::response::str(const std::string& server_name,
                                  bool persist) const
{
    return serialise(server_name, true, persist) + m_body;
}

std::string server::response::serialise(const std::string& server_name,
                                        bool with_length, bool persist,
                                        bool chunked) const
{
    std::ostringstream o;

    const std::string reason = m_reason.empty() ? reason_for(m_status) : m_reason;

    o << "HTTP/1.1 " << m_status << " " << reason << "\r\n";

    // Supplied unless the handler said otherwise, so a handler that wants to
    // lie about Content-Length -- which no correct one does -- has to say so.
    if(!m_fields.has("Date")) {
        const std::string when = http_date();

        if(!when.empty()) o << "Date: " << when << "\r\n";
    }

    if(!m_fields.has("Server") && !server_name.empty())
        o << "Server: " << server_name << "\r\n";

    // Omitted entirely when the body is not yet known -- a streaming response
    // is framed by Transfer-Encoding or by the close instead.  See
    // responder::begin.
    if(with_length && !m_fields.has("Content-Length"))
        o << "Content-Length: " << m_body.size() << "\r\n";

    // RFC 9112 6.1 forbids both at once and util::http::decide_framing refuses
    // a message carrying them, so a server that emitted both would be writing
    // what its own reader would throw out.  They are mutually exclusive above
    // and here by construction, not by checking.
    if(chunked && !m_fields.has("Transfer-Encoding"))
        o << "Transfer-Encoding: chunked\r\n";

    // Unless the handler said otherwise, and then what the caller decided.
    // Stated rather than left out: keep-alive is the HTTP/1.1 default, so
    // saying nothing would also mean persist, but a reader of a capture should
    // not have to know the version to know what this connection is doing.
    if(!m_fields.has("Connection"))
        o << "Connection: " << (persist ? "keep-alive" : "close") << "\r\n";

    for(const fields::value_type& f : m_fields) {
        // Checked on the way out, against the grammar rather than a blocklist.
        // A CR or an LF in a value is how one response becomes two, and a
        // handler builds these from whatever it was given.
        const std::string line = f.first + ": " + f.second;

        if(!util::http::grammar().at("field-line").try_parse(line))
            throw error("a handler produced a header field that cannot be sent: "
                        "\"" + line + "\"");

        o << f.first << ": " << f.second << "\r\n";
    }

    o << "\r\n";

    return o.str();
}

// -------------------------------------------------------------------- server

server::server(unsigned short port, const std::string& host,
               const sys::tls_context& tls, const sys::server::policy& p,
               const options& o)
    : m_options(o),
      m_tls(!tls.empty())
{
    m_otherwise = [](const Request&, response& r) {
        r.status(404).type("text/plain").body("not found\n");
    };

    m_transport.reset(new sys::server(
        sys::listener(port, host),
        [this](sys::socketstream& s, const sys::peer& from) { serve(s, from); },
        tls, p));
}

server::~server() = default;

unsigned short server::port() const { return m_transport->port(); }

bool server::tls() const { return m_tls; }

std::string server::url(const std::string& path) const {
    std::ostringstream o;

    // localhost rather than 127.0.0.1 for the TLS form, because a certificate
    // covers a name and the test ones cover that one.
    o << (m_tls ? "https://localhost:" : "http://127.0.0.1:") << port() << path;

    return o.str();
}

void server::route(const std::string& method, const std::string& path, handler h) {
    entry e;

    e.method = method;
    e.path = path;
    e.run = std::move(h);

    m_routes.push_back(std::move(e));
}

void server::route(const std::string& method, const std::string& path,
                   stream_handler h)
{
    entry e;

    e.method = method;
    e.path = path;
    e.stream = std::move(h);

    m_routes.push_back(std::move(e));
}

void server::otherwise(handler h) {
    if(h) m_otherwise = std::move(h);
}

bool server::serve_one(double timeout) { return m_transport->serve_one(timeout); }

void server::run() { m_transport->run(); }

void server::stop(bool drain) { m_transport->stop(drain); }

void server::join() { m_transport->join(); }

sys::server& server::transport() { return *m_transport; }

void server::responder::send(const response& r) {
    if(m_started)
        throw error("a responder sent a whole response after it had begun "
                    "streaming one");

    m_started = true;

    *m_s << r.str(m_name) << std::flush;
}

void server::responder::framing(bool chunked) {
    if(m_started)
        throw error("a responder was told how to frame a response it has "
                    "already begun");

    m_chunked = chunked;
}

void server::responder::begin(const response& head) {
    if(m_started)
        throw error("a responder began a response twice");

    m_started = true;

    // The blocking server closes after one response whatever happens, so
    // persist is false here and chunked buys only the terminator -- which is
    // what tells a client the difference between a body that ended and a
    // server that died.  See the responder class comment.
    *m_s << head.head(m_name, m_chunked, false) << std::flush;
}

void server::responder::write(const std::string& piece) {
    if(!m_started)
        throw error("a responder wrote a body piece before begin()");

    // A zero-length chunk is the terminator, so writing one here would end the
    // body early and the client would believe it complete.  Nothing to send.
    if(piece.empty()) return;

    if(m_chunked) *m_s << chunk(piece) << std::flush;
    else          *m_s << piece << std::flush;
}

void server::responder::end() {
    if(!m_started || m_ended) return;

    m_ended = true;

    if(m_chunked) *m_s << last_chunk() << std::flush;
}

bool server::responder::live() const { return m_s && bool(*m_s); }

/**
 * The request target as a path, or a reason it is not one.
 *
 * Shared by the blocking and the suspending serve, because a difference
 * between them here is a *routing* difference -- one server reaching a handler
 * the other refuses -- and that is the last place two implementations should
 * be allowed to drift.
 *
 * @return whether it decoded; `why` carries the 400's body if not
 */
bool server::path_of(const std::string& target, std::string& path,
                     std::string& why)
{
    // An encoded separator is refused rather than decoded: decoding one would
    // change how many segments the path has, which is the shape of a traversal
    // bug.  Everything else is decoded, because RFC 3986 2.1 makes "%65" and
    // "e" the same character.
    const std::string lowered = util::http::fold(target);

    if(lowered.find("%2f") != std::string::npos ||
       lowered.find("%5c") != std::string::npos) {
        why = "an encoded path separator in the request target\n";

        return false;
    }

    try {
        util::URL u;

        u.parse_reference(target);

        path = util::uri::decode(u.get_path());
    }
    catch(std::exception&) {
        why = "not a request target\n";

        return false;
    }

    return true;
}

const server::entry* server::route_for(const std::string& method,
                                       const std::string& path) const
{
    for(const entry& e : m_routes) {
        if(e.method == method && e.path == path) return &e;
    }

    return 0;
}

void server::serve(sys::socketstream& s, const sys::peer&) {
    response r;

    Request q;

    try {
        q = util::http::read_request_head(s, m_options.max_head);

        // RFC 9110 10.1.1.  curl sends this for any POST over about a kilobyte
        // and then waits for it; a server that ignores it makes every such
        // client wait out its own timeout before sending the body.
        if(util::http::fold(q.fields().get("Expect")) == "100-continue") {
            s << "HTTP/1.1 100 Continue\r\n\r\n" << std::flush;
        }

        q.set_body(util::http::read_body(s, q.body_framing(), q.content_length(),
                                         m_options.max_body));
    }
    catch(util::http::error& e) {
        // A message this cannot read at all.  Answer, so a client learns
        // something rather than seeing a bare close, and stop.
        r.status(400).type("text/plain").body(std::string(e.what()) + "\n");

        s << r.str(m_options.server_name) << std::flush;

        return;
    }

    // The path, without the query.  parse_reference is what learned to read one
    // of these two branches ago.
    std::string path;
    std::string why;

    if(!path_of(q.target(), path, why)) {
        r.status(400).type("text/plain").body(why);

        s << r.str(m_options.server_name) << std::flush;

        return;
    }

    const entry* chosen = route_for(q.method(), path);

    // A streaming route takes a different path entirely, because the promise
    // the buffered one makes -- that a handler which throws is still answered
    // -- cannot be kept once bytes have gone.
    // Registered for an async server, reached on a blocking one.  Refused
    // where it is reached, so one route table can be built for both modes.
    if(chosen && chosen->async_stream) {
        response oops;

        oops.status(500).type("text/plain")
            .body("that route has a suspending handler and this server is "
                  "blocking\n");

        s << oops.str(m_options.server_name) << std::flush;

        return;
    }

    if(chosen && chosen->stream) {
        responder out(s, m_options.server_name);

        // Chunked for an HTTP/1.1 client, the close for anything older.  This
        // server closes after one response either way, so what chunked buys
        // here is only the terminator -- and that is not nothing: without it a
        // client cannot tell a body that ended from a server that died.
        out.framing(q.version() == "HTTP/1.1");

        try {
            chosen->stream(q, out);
        }
        catch(...) {
            if(!out.started()) {
                response oops;

                oops.status(500).type("text/plain").body("internal error\n");

                s << oops.str(m_options.server_name) << std::flush;
            }

            // Otherwise there is nothing to say: the status went out long ago
            // and the client sees the close as a truncated body.  Rethrown
            // either way, so sys::server::on_error still learns of it.
            throw;
        }

        // A handler that produced nothing at all is a bug in the handler, and
        // a bare close would look like a crash.
        if(!out.started()) {
            response oops;

            oops.status(500).type("text/plain")
                .body("the handler produced no response\n");

            s << oops.str(m_options.server_name) << std::flush;
        }
        else {
            // **Only on this path.**  A handler that threw took the catch
            // above and was rethrown out of this function, so it never reaches
            // here and its body is never terminated -- which is exactly what
            // tells the client the response is unfinished.
            out.end();
        }

        return;
    }

    // Nothing has reached the socket yet, so a handler that throws can still be
    // answered -- which is the whole reason a response is accumulated rather
    // than streamed.
    //
    // Serialising it is inside the try as well, and that is not fussiness:
    // str() refuses a field a handler built that cannot be sent, and without
    // this the client would get a bare close, which is indistinguishable from
    // a crash.  A refused field is the server's fault and 500 is what says so.
    std::string answer;

    try {
        (chosen ? chosen->run : m_otherwise)(q, r);

        answer = r.str(m_options.server_name);
    }
    catch(...) {
        response oops;

        oops.status(500).type("text/plain").body("internal error\n");

        s << oops.str(m_options.server_name) << std::flush;

        // Rethrown, so sys::server::on_error sees it: answering the client is
        // not the same as the failure having been dealt with.
        throw;
    }

    s << answer << std::flush;
}


// ---------------------------------------------------------- the async server

sys::task<void> server::async_responder::send(const response& r) {
    if(m_started)
        throw error("a responder sent a whole response after it had begun "
                    "streaming one");

    m_started = true;

    // **A whole response carries its own length, so it can be followed.**
    // This used to serialise with str()'s default, which is close -- so every
    // route that could stream but answered whole shut the connection, and
    // jserve's non-streaming completions did exactly that.  Found by pointing
    // curl at two of them and watching the second say close.
    m_persist = m_want_persist;

    co_await m_writer->write(r.str(m_name, m_persist));
}

sys::task<void> server::async_responder::send_serialised(const std::string& wire) {
    if(m_started)
        throw error("a responder sent a whole response after it had begun "
                    "streaming one");

    m_started = true;

    co_await m_writer->write(wire);
}

void server::async_responder::framing(bool chunked, bool persist) {
    if(m_started)
        throw error("a responder was told how to frame a response it has "
                    "already begun");

    m_chunked = chunked;

    // Recorded, not resolved.  Whether the connection can carry another
    // request depends on how *this* response turns out to be framed, and that
    // is not known until the handler chooses between begin() and send():
    // a streamed body needs chunked to have an end, a whole one has a
    // Content-Length and needs nothing.
    m_want_persist = persist;
}

sys::task<void> server::async_responder::begin(const response& head) {
    if(m_started) throw error("a responder began a response twice");

    m_started = true;
    m_begun = true;

    // Only now: a streamed body is followable exactly when chunked gives it an
    // end.  framing() deliberately does not decide this.
    m_persist = m_want_persist && m_chunked;

    co_await m_writer->write(head.head(m_name, m_chunked, m_persist));
}

sys::task<void> server::async_responder::write(const std::string& piece) {
    if(!m_started)
        throw error("a responder wrote a body piece before begin()");

    // A zero-length chunk is the terminator; see the note on the declaration.
    if(piece.empty()) co_return;

    co_await m_writer->write(m_chunked ? chunk(piece) : piece);
}

sys::task<void> server::async_responder::end() {
    // **begun(), not started().**  send() also marks a responder started, and
    // a whole response has already ended -- terminating it again would put a
    // zero-length chunk after a body with a Content-Length, which the next
    // response on the connection would then be read as starting inside.
    //
    // That was live and invisible: the same bug that made send() say close
    // also guaranteed nothing came after the stray chunk to be corrupted by it.
    if(!m_begun || m_ended) co_return;

    m_ended = true;

    if(m_chunked) co_await m_writer->write(last_chunk());
}

server::server(async_t, unsigned short port, const std::string& host,
               const sys::tls_context& tls, const sys::server::policy& p,
               const options& o)
    : m_options(o),
      m_tls(!tls.empty()),
      m_async(true),
      m_request_timeout(p.io_timeout)
{
    m_otherwise = [](const Request&, response& r) {
        r.status(404).type("text/plain").body("not found\n");
    };

    m_transport.reset(new sys::server(
        sys::listener(port, host),
        sys::server::async_handler(
            [this](sys::server::connection& c, const sys::peer& from)
                -> sys::task<void> {
                co_await serve_async(c, from);
            }),
        tls, p));
}

void server::route(const std::string& method, const std::string& path,
                   async_stream_handler h)
{
    entry e;

    e.method = method;
    e.path = path;
    e.async_stream = std::move(h);

    m_routes.push_back(std::move(e));
}

namespace {

    /**
     * The request deadline, disarmed however the request ends.
     *
     * cancel() is called explicitly partway through, because a deadline on
     * *reading the request* must not still be running while a slow handler
     * produces the answer.  That call was here before this and was sufficient,
     * because every path that got past the read reached it.
     *
     * Keep-alive adds paths that do not.  A connection can now end *inside*
     * the read -- a client saying goodbye between requests -- and that is the
     * ordinary way a persistent connection finishes rather than an edge case.
     * A timer left behind there sits in the reactor holding a copy of the
     * token for the rest of its bound, once for every connection a client ever
     * walked away from.
     *
     * Hence a destructor, and an idempotent cancel() for the path that still
     * wants to disarm early.  Between them no timer outlives the request that
     * armed it on any path the reactor's thread takes, which is what keeps
     * sys::deadline's own warning -- that an uncancelled timer fires "into the
     * *next* operation on the same token" -- unreachable on a connection that
     * now *has* a next operation.
     *
     * The exception, and it is not a small one, is in cancel() below: a
     * destructor can run on a thread that must not touch the reactor, and
     * there the timer is abandoned rather than cancelled.
     */
    class armed_deadline {
    public:
        explicit armed_deadline(sys::reactor& r) : m_reactor(&r) {}

        ~armed_deadline() { cancel(); }

        armed_deadline(const armed_deadline&) = delete;
        armed_deadline& operator=(const armed_deadline&) = delete;

        /** Replaces whatever was armed before; seconds <= 0 arms nothing. */
        void arm(double seconds, sys::cancel_token t) {
            cancel();

            if(seconds > 0) {
                m_timer = sys::deadline(
                    *m_reactor,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::duration<double>(seconds)),
                    t);
            }
        }

        void cancel() {
            if(m_timer == sys::reactor::timer_token::none) return;

            // **Only ever from the reactor's own thread**, and this guard is
            // the whole reason a destructor here is safe at all.
            //
            // A destructor runs wherever the frame is destroyed, and one of
            // the paths that destroys this frame is a cancellation: stop()
            // ends a parked connection by requesting its token, and it does
            // that on the *calling* thread, so the coroutine unwinds there
            // while the reactor thread is still running a pass.
            // reactor::cancel() mutates two containers with no lock, so
            // cancelling from that unwind is a data race -- and the symptom is
            // heap corruption a long way from the cause, which is how this was
            // found rather than reasoned about.
            //
            // Abandoning the timer instead is safe and cheap: it fires later
            // into a token that has already been requested, and requesting a
            // requested token does nothing.  That is also exactly what the
            // code before this class did on the same path, by not cancelling
            // at all.
            if(m_reactor->on_reactor_thread()) m_reactor->cancel(m_timer);

            m_timer = sys::reactor::timer_token::none;
        }

    private:
        sys::reactor*             m_reactor;
        sys::reactor::timer_token m_timer = sys::reactor::timer_token::none;
    };

}

sys::task<void> server::serve_async(sys::server::connection& c,
                                    const sys::peer& from)
{
    // **Keep-alive, and why the blocking server does not get it.**
    //
    // A persistent connection spends most of its life idle, waiting for a
    // request that may never come.  In serve() that idle time would be a
    // *thread*: a handful of clients holding connections open would occupy
    // every worker in the pool and the server would stop accepting -- so
    // keep-alive there is a denial of service a server performs on itself.
    // Here an idle connection is a suspended coroutine and a registration in
    // the reactor.  It holds a descriptor and no thread.
    //
    // So this is something the asynchronous server can do and the blocking one
    // cannot, rather than a feature missing from the blocking one.  It is also
    // the first thing in this tree that is *only* worth having because of #4.
    std::size_t served = 0;

    while(co_await serve_request_async(c, from, served)) {
        ++served;
    }
}

sys::task<bool> server::serve_request_async(sys::server::connection& c,
                                            const sys::peer&,
                                            std::size_t served)
{
    async_responder out(c.writer(), m_options.server_name, c.reactor(),
                        c.pool());

    // **The slow-loris bound.**  Every piece of this existed before and
    // nothing armed it: the connection is a coroutine carrying a token, the
    // token now ends a wait rather than merely marking it (#212), and a
    // deadline is a timer that requests one.  This is what uses them.
    //
    // Over the whole request read, not per operation -- which is the
    // difference from the blocking server's SO_RCVTIMEO, and is what makes a
    // client sending one octet every twenty-nine seconds a dropped connection
    // here and an indefinite one there.
    //
    // **Waiting for a request to start is a different question from reading
    // one, and they get different answers.**  Three of them:
    //
    //   a new connection, silent           initial_idle_timeout   5s
    //   a request, once it has started     io_timeout            30s
    //   a reused connection, between       idle_timeout          60s
    //
    // The middle one is the only one that is really a *request* timeout; the
    // other two bound silence at two points where silence means different
    // things.  A client that has just connected is about to speak, so five
    // seconds is generous.  A client that polls every fifteen seconds has a
    // perfectly good reason to be quiet, and a five second bound there would
    // hand it a new connection every time and make keep-alive an elaborate way
    // of changing nothing.
    //
    // Collapsing any two of these means one of them is wrong: an earlier draft
    // used a single timer per request and so applied the *idle* bound to the
    // whole of every request after the first, which gave a large second POST
    // five seconds where the first got thirty.
    armed_deadline limit(c.reactor());

    Request q;

    // **co_await cannot appear in a catch handler.**  That is a language rule,
    // not a limitation of anything here, and it shapes every error path below:
    // the failure is recorded, the catch ends, and the answer is written
    // afterwards.  The blocking serve() writes from inside its catch, which
    // reads more directly and is not available here.
    std::string bad;

    try {
        // Phase one: nothing has arrived yet, so this is silence rather than a
        // slow request, and which silence it is depends on whether this
        // connection has ever been used.
        //
        // fill() rather than sys::readable() on the descriptor, because for a
        // TLS connection those are **different questions** -- the socket can be
        // ready with a record that yields no plaintext, and plaintext can be
        // waiting with the socket quiet.  async_tls answers the one that
        // matters and readable() would answer the other.
        if(c.reader().buffered() == 0) {
            armed_deadline idle(c.reactor());

            idle.arm(served == 0 ? m_options.initial_idle_timeout
                                 : m_options.idle_timeout,
                     c.token());

            // The answer is deliberately dropped.  read_head_if_any below is
            // the single place that decides what an ended connection means,
            // and two places deciding that is how they come to disagree.
            (void) co_await c.reader().fill();
        }

        // Phase two: something is here, so from now on this is a request being
        // read and the request bound applies -- the same one the first request
        // on the connection got, reset per request the way a fresh connection
        // would have got it.
        limit.arm(m_request_timeout, c.token());

        // **The polite goodbye**, which a closing server never had to know
        // about: a peer that shuts the connection between requests has not
        // sent a broken message, it has finished.  read_head would call that
        // a head that ended after zero octets and this would answer 400 down
        // a socket that is already gone.
        std::string head;

        if(!co_await util::http::read_head_if_any(c.reader(),
                                                  m_options.max_head, head)) {
            co_return false;
        }

        // Read on the reactor's thread, because that is where the descriptor
        // is.  **Parsed on a worker**, because it is not: parsing a head of
        // eight fields measures at 59us, and every microsecond of it is a
        // microsecond no other connection is being dispatched.  The hop costs
        // 5.85us round trip, so it pays for itself at about one field.
        //
        // read_head and parse_request_head were already separate -- the
        // RFC-grammar work split framing from parsing for its own reasons --
        // so this is a hop between two calls that already existed.
        co_await sys::on_pool(c.pool());

        q = util::http::parse_request_head(head);

        co_await sys::on_reactor(c.reactor());

        // RFC 9110 10.1.1.  curl sends this for any POST over about a
        // kilobyte and then waits for it; a server that ignores it makes
        // every such client wait out its own timeout before sending the body.
        if(util::http::fold(q.fields().get("Expect")) == "100-continue")
            co_await c.writer().write("HTTP/1.1 100 Continue\r\n\r\n");

        // **Read whole, which is also what leaves the connection at a message
        // boundary.**  A body this did not consume would still be in the
        // reader, and the next read_head would parse the client's leftover
        // octets as a request line -- which is the smuggle, arriving by way
        // of a server that simply forgot to finish reading.
        q.set_body(co_await util::http::read_body(c.reader(), q.body_framing(),
                                                  q.content_length(),
                                                  m_options.max_body));
    }
    catch(util::http::error& e) {
        // A message this cannot read at all.  Answer, so a client learns
        // something rather than seeing a bare close, and stop.
        bad = std::string(e.what()) + "\n";
    }
    catch(sys::cancelled&) {
        // The deadline, or a shutdown.  Nothing is sent: the client has not
        // finished asking, and a 408 down a connection whose peer is still
        // mid-request is as likely to be missed as read.  The connection
        // closes when this returns.
        co_return false;
    }

    // In hand.  A handler that takes a long time to answer is a different
    // question, and leaving this armed would turn a slow reply into a dropped
    // connection.
    limit.cancel();

    // **Every 400 closes, and the framing ones have to.**  Where this message
    // ended is exactly what failed, so where the next one starts is not
    // something this can claim to know either -- reading on would mean taking
    // whatever the client put there as a request, which is the smuggle.
    //
    // A bad request-target is a well-framed message and could be answered on a
    // connection that continues.  It is not, because one rule is easier to be
    // sure of than two, and a client sending one is not in a conversation
    // worth the saved handshake.
    if(!bad.empty()) {
        response r;

        r.status(400).type("text/plain").body(bad);

        co_await out.send(r);

        co_return false;
    }

    std::string path;

    if(!path_of(q.target(), path, bad)) {
        response r;

        r.status(400).type("text/plain").body(bad);

        co_await out.send(r);

        co_return false;
    }

    const entry* chosen = route_for(q.method(), path);

    // A route registered for the blocking streaming handler cannot run here:
    // responder writes to a socketstream and there is not one.  Refused where
    // it is reached rather than where it was registered, so one route table
    // can be built for both modes.
    if(chosen && chosen->stream) {
        response oops;

        oops.status(500).type("text/plain")
            .body("that route has a blocking streaming handler and this "
                  "server is asynchronous\n");

        co_await out.send(oops);

        co_return false;
    }

    if(chosen && chosen->async_stream) {
        // Chunked needs an HTTP/1.1 client; anything older gets the close, and
        // gets it as the framing rather than as an afterthought.
        const bool can_chunk = q.version() == "HTTP/1.1";

        // Note what this does *not* ask: whether the body can be framed so
        // that something may follow it.  framing() is the one place that
        // reconciles the two, and out.persist() below is its answer -- so
        // there is a single place to be wrong rather than two that must agree.
        const bool persist = m_options.keep_alive &&
                             util::http::persistent(q) &&
                             served + 1 < m_options.max_requests;

        out.framing(can_chunk, persist);

        // Held rather than rethrown with a bare `throw;` outside the catch:
        // by then there is no exception in flight and a bare throw calls
        // std::terminate.  That is the sharp edge the co_await-in-a-catch rule
        // creates, and it is a crash rather than a diagnostic.
        std::exception_ptr threw;

        try {
            co_await chosen->async_stream(q, out);
        }
        catch(...) {
            threw = std::current_exception();
        }

        // **Snapshot before anything below sends a 500**, because send() marks
        // the responder started too -- and a 500 is a whole Content-Length
        // response that says close, not a streamed body that can be continued
        // from.  Conflating the two would keep a connection open after telling
        // the client it was closing.
        const bool streamed = out.started();

        if(threw && !out.started()) {
            response oops;

            oops.status(500).type("text/plain").body("internal error\n");

            co_await out.send(oops);
        }

        // A handler that produced nothing at all is a bug in the handler, and
        // a bare close would look like a crash.
        if(!threw && !out.started()) {
            response oops;

            oops.status(500).type("text/plain")
                .body("the handler produced no response\n");

            co_await out.send(oops);
        }

        // Terminated only when the handler returned.  A handler that failed
        // leaves the body unfinished on purpose: the status left long ago and
        // cannot be taken back, so the one thing still available to say "this
        // is not the whole response" is to not write the terminator.
        if(!threw && streamed) co_await out.end();

        // Rethrown so sys::server::on_error still learns of it.  Once begin()
        // has gone out there is nothing to *say* -- which is what responder
        // gave up in #200 and is no different here.
        if(threw) std::rethrow_exception(threw);

        // And now a streaming response can be followed by another request,
        // which is what the terminating chunk bought.  Asked of the responder
        // rather than recomputed: it is what actually went out on the wire.
        co_return streamed && out.persist();
    }

    // The buffered case, and the common one.  **The handler is not a
    // coroutine**: it fills a response and cannot suspend, so the same handler
    // runs on both servers -- which is what lets the two be compared on one
    // route table.
    // **The handler and the serialisation, on a worker.**
    //
    // The parsing and serialising are the *floor*, and they are measurable:
    // a head of eight fields parses in 59us and a response of ten fields
    // serialises in 52us, because str() checks every field against the
    // grammar on the way out.  That is 111us on the reactor's thread for a
    // handler that does nothing at all, against 5.85us for a hop round trip.
    //
    // **But the handler is the reason.**  A buffered handler is arbitrary
    // code: a file server reads a file, an application server queries
    // something, a template gets rendered.  That is milliseconds, not
    // microseconds -- and without this hop every one of them runs on the one
    // thread that dispatches every other connection, so a single handler
    // reading a slow disk stops the whole server for as long as it takes.
    //
    // The microseconds justify the hop for a trivial handler.  The
    // milliseconds are why it matters.
    //
    // A *streaming* handler is not hopped, above: it does its own writing, so
    // it has to be where the writer is, and a handler that wants the pool for
    // part of its work can co_await on_pool itself.
    //
    // With policy::threads == 0 the queue runs a job on the thread that posted
    // it, so all of this is inline and a serial server pays nothing.
    co_await sys::on_pool(c.pool());

    response r;

    std::exception_ptr threw;

    try {
        (chosen ? chosen->run : m_otherwise)(q, r);
    }
    catch(...) {
        threw = std::current_exception();
    }

    // **Decided from the request, then offered to the handler.**  The request
    // says whether the client is willing; max_requests says whether this
    // connection has had its share; and a handler that set its own
    // `Connection: close` overrides both, because it may know something about
    // what it just sent that this does not.
    //
    // The decision has to be made here rather than after the write, because it
    // is a field in the very response being serialised: a server that closed a
    // connection it had just told the client to keep is worse than one that
    // never offered.
    bool persist = m_options.keep_alive &&
                   util::http::persistent(q) &&
                   served + 1 < m_options.max_requests;

    if(persist && util::http::connection_close(r.fields())) persist = false;

    // Serialised here, on the worker, rather than as the argument to write()
    // on the reactor -- which is what it was, and is the half of this that is
    // easy to miss.
    std::string wire;

    if(!threw) wire = r.str(m_options.server_name, persist);

    co_await sys::on_reactor(c.reactor());

    if(threw) {
        response oops;

        oops.status(500).type("text/plain").body("internal error\n");

        co_await out.send(oops);

        std::rethrow_exception(threw);
    }

    co_await out.send_serialised(wire);

    co_return persist;
}

}
}
}
