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

#include <jlib/util/URL.hh>
#include <jlib/util/util.hh>

#include <ctime>
#include <sstream>

namespace jlib {
namespace net {
namespace http {

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

std::string server::response::str(const std::string& server_name) const {
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

    if(!m_fields.has("Content-Length"))
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

    o << "\r\n" << m_body;

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

void server::otherwise(handler h) {
    if(h) m_otherwise = std::move(h);
}

bool server::serve_one(double timeout) { return m_transport->serve_one(timeout); }

void server::run() { m_transport->run(); }

void server::stop() { m_transport->stop(); }

void server::join() { m_transport->join(); }

sys::server& server::transport() { return *m_transport; }

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

    {
        const std::string& target = q.target();

        // An encoded separator is refused rather than decoded: decoding one
        // would change how many segments the path has, which is the shape of a
        // traversal bug.  Everything else is decoded, because RFC 3986 2.1
        // makes "%65" and "e" the same character.
        const std::string lowered = util::http::fold(target);

        if(lowered.find("%2f") != std::string::npos ||
           lowered.find("%5c") != std::string::npos) {
            r.status(400).type("text/plain")
             .body("an encoded path separator in the request target\n");

            s << r.str(m_options.server_name) << std::flush;

            return;
        }

        try {
            util::URL u;

            u.parse_reference(target);

            path = util::uri::decode(u.get_path());
        }
        catch(std::exception&) {
            r.status(400).type("text/plain").body("not a request target\n");

            s << r.str(m_options.server_name) << std::flush;

            return;
        }
    }

    const handler* chosen = 0;

    for(const entry& e : m_routes) {
        if(e.method == q.method() && e.path == path) {
            chosen = &e.run;
            break;
        }
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
        (chosen ? *chosen : m_otherwise)(q, r);

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

}
}
}
