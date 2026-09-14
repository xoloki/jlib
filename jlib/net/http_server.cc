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

std::string server::response::head(const std::string& server_name) const {
    return serialise(server_name, false);
}

std::string server::response::str(const std::string& server_name) const {
    return serialise(server_name, true) + m_body;
}

std::string server::response::serialise(const std::string& server_name,
                                        bool with_length) const
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
    // is delimited by the close instead.  See responder::begin.
    if(with_length && !m_fields.has("Content-Length"))
        o << "Content-Length: " << m_body.size() << "\r\n";

    // Always.  One request per connection takes every keep-alive framing
    // question off the table, and those questions are where smuggling lives.
    if(!m_fields.has("Connection")) o << "Connection: close\r\n";

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

void server::responder::begin(const response& head) {
    if(m_started)
        throw error("a responder began a response twice");

    m_started = true;

    // head() omits Content-Length, so the body is delimited by the close --
    // which this server sends on every response anyway.
    *m_s << head.head(m_name) << std::flush;
}

void server::responder::write(const std::string& piece) {
    if(!m_started)
        throw error("a responder wrote a body piece before begin()");

    *m_s << piece << std::flush;
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

    co_await m_writer->write(r.str(m_name));
}

sys::task<void> server::async_responder::begin(const response& head) {
    if(m_started) throw error("a responder began a response twice");

    m_started = true;

    // head() omits Content-Length, so the body is delimited by the close --
    // which this server sends on every response anyway.
    co_await m_writer->write(head.head(m_name));
}

sys::task<void> server::async_responder::write(const std::string& piece) {
    if(!m_started)
        throw error("a responder wrote a body piece before begin()");

    co_await m_writer->write(piece);
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

sys::task<void> server::serve_async(sys::server::connection& c,
                                    const sys::peer&)
{
    async_responder out(c.writer(), m_options.server_name);

    // **The slow-loris bound.**  Every piece of this existed before and
    // nothing armed it: the connection is a coroutine carrying a token, the
    // token now ends a wait rather than merely marking it (#212), and a
    // deadline is a timer that requests one.  This is the line that uses them.
    //
    // Over the whole request read, not per operation -- which is the
    // difference from the blocking server's SO_RCVTIMEO, and is what makes a
    // client sending one octet every twenty-nine seconds a dropped connection
    // here and an indefinite one there.
    sys::reactor::timer_token limit = sys::reactor::timer_token::none;

    if(m_request_timeout > 0) {
        limit = sys::deadline(
            c.reactor(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double>(m_request_timeout)),
            c.token());
    }

    Request q;

    // **co_await cannot appear in a catch handler.**  That is a language rule,
    // not a limitation of anything here, and it shapes every error path below:
    // the failure is recorded, the catch ends, and the answer is written
    // afterwards.  The blocking serve() writes from inside its catch, which
    // reads more directly and is not available here.
    std::string bad;

    try {
        q = co_await util::http::read_request_head(c.reader(),
                                                   m_options.max_head);

        // RFC 9110 10.1.1.  curl sends this for any POST over about a
        // kilobyte and then waits for it; a server that ignores it makes
        // every such client wait out its own timeout before sending the body.
        if(util::http::fold(q.fields().get("Expect")) == "100-continue")
            co_await c.writer().write("HTTP/1.1 100 Continue\r\n\r\n");

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
        co_return;
    }

    // In hand.  A handler that takes a long time to answer is a different
    // question, and leaving this armed would turn a slow reply into a dropped
    // connection.
    if(limit != sys::reactor::timer_token::none) c.reactor().cancel(limit);

    if(!bad.empty()) {
        response r;

        r.status(400).type("text/plain").body(bad);

        co_await out.send(r);

        co_return;
    }

    std::string path;

    if(!path_of(q.target(), path, bad)) {
        response r;

        r.status(400).type("text/plain").body(bad);

        co_await out.send(r);

        co_return;
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

        co_return;
    }

    if(chosen && chosen->async_stream) {
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

        // Rethrown so sys::server::on_error still learns of it.  Once begin()
        // has gone out there is nothing to *say* -- the status left long ago
        // and the client sees the close as a truncated body -- which is what
        // responder gave up in #200 and is no different here.
        if(threw) std::rethrow_exception(threw);

        co_return;
    }

    // The buffered case, and the common one.  **The handler is not a
    // coroutine**: it fills a response and cannot suspend, so the same handler
    // runs on both servers -- which is what lets the two be compared on one
    // route table.
    response r;

    std::exception_ptr threw;

    try {
        (chosen ? chosen->run : m_otherwise)(q, r);
    }
    catch(...) {
        threw = std::current_exception();
    }

    if(threw) {
        response oops;

        oops.status(500).type("text/plain").body("internal error\n");

        co_await out.send(oops);

        std::rethrow_exception(threw);
    }

    co_await out.send(r);
}

}
}
}
