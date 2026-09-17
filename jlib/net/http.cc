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

#include <jlib/net/http.hh>
#include <jlib/net/net.hh>

#include <jlib/sys/proxystream.hh>
#include <jlib/sys/socketstream.hh>
#include <jlib/sys/sslproxystream.hh>
#include <jlib/sys/sslstream.hh>

#include <jlib/util/abnf.hh>
#include <jlib/util/util.hh>

#include <cstdlib>
#include <memory>
#include <sstream>

namespace jlib {
namespace net {
namespace http {

/**
 * "jlib/" and the release, from the build rather than from a literal.
 *
 * JLIB_RELEASE_STRING comes from AM_CPPFLAGS, which is why this is here and
 * not in the header: the header is installed, and a consumer compiling against
 * it has no such macro.  The literal it replaced said 1.2 for as long as it
 * took somebody to notice, which was after the 2.0.0 release went out.
 */
std::string default_user_agent() { return "jlib/" JLIB_RELEASE_STRING; }

namespace {

    const char* const HEX = "0123456789ABCDEF";

    bool debug() { return std::getenv("JLIB_NET_HTTP_DEBUG") != 0; }

    /** Whether a caller's field name and value can safely go on the wire. */
    void check_field(const std::string& name, const std::string& value) {
        // Against the grammar, not against a list of bad characters.  What
        // this is really stopping is a CR or an LF in a value, because that is
        // how one request becomes two -- but field-name being a token and
        // field-value being field-content are the rules that say so, and
        // checking them is both stricter and easier to defend than a blocklist.
        const std::string line = name + ": " + value;

        if(!util::http::grammar().at("field-line").try_parse(line))
            throw error("not a header field: \"" + line + "\"");
    }

    /** The Host field: authority without userinfo, port only when it is not the default. */
    std::string host_field(const util::URL& url, unsigned int port, bool tls) {
        std::ostringstream o;

        // A literal goes back in its brackets: RFC 9110 4.1 writes uri-host,
        // and an IPv6 address without them is ambiguous with the port.
        if(url.get_host().find(':') != std::string::npos)
            o << "[" << url.get_host() << "]";
        else
            o << url.get_host();

        if(port != (tls ? 443u : 80u)) o << ":" << port;

        return o.str();
    }

    /**
     * The request-target: origin-form, per RFC 9112 3.2.1.
     *
     * get_path() alone is not it.  URL keeps the path and the query apart, so
     * a target built from the path drops "?code=..." -- which is the whole
     * message for half the requests this client makes.  And an empty path goes
     * out as "/": RFC 9112 3.2.1 says so explicitly, and a request line
     * reading "GET  HTTP/1.1" is not a request line at all.
     *
     * The fragment is never sent (RFC 9110 7.1).  It is the client's, not the
     * server's.
     */
    std::string request_target(const util::URL& url) {
        std::string target = url.get_path();

        if(target.empty()) target = "/";

        const std::string qs = url.get_qs();

        if(!qs.empty()) target += "?" + qs;

        return target;
    }

    /** Scheme, host and port, for comparing two URLs and for saying which. */
    std::string origin_text(const util::URL& u) {
        const bool tls = u.get_protocol() == "https";

        const unsigned int port = u.get_port().empty()
                                  ? (tls ? 443u : 80u) : u.get_port_val();

        return u.get_protocol() + "://" + u.get_host() + ":" +
               std::to_string(port);
    }

    bool same_origin(const util::URL& a, const util::URL& b) {
        return origin_text(a) == origin_text(b);
    }

    /**
     * Whether the connection survives this response.
     *
     * RFC 9112 9.3: HTTP/1.1 is persistent unless the response says close;
     * HTTP/1.0 is the other way round and needs to ask.  One more makes it
     * false: a body framed by the connection ending has just ended it.
     *
     * A sink that stopped early also kills the connection, and that is *not*
     * decided here -- read_body returns void and cannot say whether it
     * reached the end, so the caller watches its own sink.  See send().
     */
    bool persists(const Response& r, const std::string& method) {
        const std::string conn = util::http::fold(r.fields().get("Connection"));

        std::string lower;

        for(char c : conn) lower += char(std::tolower((unsigned char)(c)));

        if(lower.find("close") != std::string::npos) return false;

        if(r.version() == "HTTP/1.0" &&
           lower.find("keep-alive") == std::string::npos) return false;

        if(method != "HEAD" && r.body_framing() == util::http::framing::until_close)
            return false;

        return true;
    }

    std::unique_ptr<sys::socketstream> transport(const util::URL& url,
                                                 unsigned int port,
                                                 bool tls,
                                                 const options& o)
    {
        std::string phost;
        unsigned int pport = 0;

        const bool proxied = proxy_of(url, phost, pport);

        std::unique_ptr<sys::socketstream> sock;

        if(proxied) {
            if(tls) sock.reset(new sys::tlsproxystream(url.get_host(), port, phost, pport));
            else    sock.reset(new sys::proxystream(url.get_host(), port, phost, pport));
        }
        else {
            if(tls) sock.reset(new sys::tlsstream(url.get_host(), port));
            else    sock.reset(new sys::socketstream(url.get_host(), port));
        }

        // The library default is no read timeout, for the reason written down
        // in sys.hh: Imap4::idle() is supposed to sit on a quiet socket for
        // half an hour.  An HTTP client is not, and this is the caller that
        // knows its own timing.
        if(o.timeout > 0) sock->set_timeout(o.timeout);

        return sock;
    }

    Response once(const std::string& method,
                  const util::URL& url,
                  const fields& send,
                  const std::string& body,
                  const options& o)
    {
        if(!util::http::grammar().at("method").try_parse(method))
            throw error("not an HTTP method: \"" + method + "\"");

        const std::string scheme = url.get_protocol();
        const bool tls = scheme == "https";

        if(scheme != "http" && !tls)
            throw error("not an HTTP URL: \"" + url.coagulate() + "\"");

        if(url.get_host().empty())
            throw error("an HTTP URL with no host: \"" + url.coagulate() + "\"");

        const unsigned int port = url.get_port().empty()
                                  ? (tls ? 443u : 80u)
                                  : url.get_port_val();

        // The caller's fields are checked before anything is connected.  They
        // cannot become a valid request however the socket goes, so opening
        // one first only means a connection the server has to notice, time out
        // and clean up on behalf of a request that was never going to be sent.
        for(const fields::value_type& f : send) check_field(f.first, f.second);

        std::unique_ptr<sys::socketstream> sock = transport(url, port, tls, o);

        std::ostringstream head;

        head << method << " " << request_target(url) << " HTTP/1.1\r\n";

        // Supplied unless the caller said otherwise.
        //
        // Connection: close is not politeness.  It ends the message at the end
        // of the connection and takes every keep-alive framing question off the
        // table -- and those questions are exactly where request smuggling
        // lives.  One request per connection is also all this client needs.
        //
        // Accept-Encoding: identity, because RFC 9110 12.5.3 lets a server
        // send any coding to a client that sends no Accept-Encoding at all.
        // Nothing here can inflate a gzip body, so asking for one and then
        // handing the caller compressed bytes it will try to parse as JSON is
        // a failure worth ruling out rather than diagnosing later.
        fields out;

        out.add("Host", host_field(url, port, tls));
        out.add("User-Agent", o.user_agent);
        out.add("Connection", "close");
        out.add("Accept-Encoding", "identity");

        if(!body.empty()) out.add("Content-Length", std::to_string(body.size()));

        for(const fields::value_type& f : out) {
            if(!send.has(f.first)) head << f.first << ": " << f.second << "\r\n";
        }

        for(const fields::value_type& f : send) {
            head << f.first << ": " << f.second << "\r\n";
        }

        head << "\r\n";

        if(debug()) std::cerr << head.str() << std::flush;

        *sock << head.str() << body << std::flush;

        if(!*sock) throw error("the connection failed while sending the request");

        // HEAD has no body however the fields are written (RFC 9112 6.3), and
        // this is the only place that knows which method was sent.
        Response r = util::http::read_response_head(*sock, method == "HEAD");

        r.set_body(util::http::read_body(*sock, r, o.max_body));

        if(debug())
            std::cerr << r.status() << " " << r.reason()
                      << ", " << r.body().size() << " octets of body" << std::endl;

        return r;
    }

}

std::string form_encode(const std::map<std::string, std::string>& form) {
    std::string out;

    for(const std::pair<const std::string, std::string>& kv : form) {
        if(!out.empty()) out += "&";

        for(int half = 0; half < 2; half++) {
            const std::string& s = half == 0 ? kv.first : kv.second;

            for(unsigned char c : s) {
                // The unreserved set of RFC 3986 2.3, which is also what this
                // encoding leaves alone.
                if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') ||
                   c == '-' || c == '.' || c == '_' || c == '~') {
                    out += static_cast<char>(c);
                }
                else if(c == ' ') {
                    // The one difference from uri::encode, and the reason this
                    // exists: HTML's form encoding spells a space "+".  A "+"
                    // in the data therefore has to be escaped, which is the
                    // case that bites -- base64 produces them constantly, and
                    // a token sent through the wrong encoder arrives with
                    // spaces in it and does not match.
                    out += '+';
                }
                else {
                    out += '%';
                    out += HEX[c >> 4];
                    out += HEX[c & 0x0f];
                }
            }

            if(half == 0) out += "=";
        }
    }

    return out;
}

// ---------------------------------------------------------------- connection

connection::connection(const util::URL& origin, const options& o)
    : m_origin(origin), m_o(o)
{
}

connection::~connection() {}

void connection::close() {
    m_sock.reset();

    m_live = false;
    m_served = 0;
}

Response connection::request(const std::string& method, const util::URL& target,
                             const fields& send, const std::string& body)
{
    return this->send(method, target, send, body, 0);
}

Response connection::request(const std::string& method, const util::URL& target,
                             const fields& send, const std::string& body,
                             const util::http::body_sink& sink)
{
    return this->send(method, target, send, body, &sink);
}

Response connection::send(const std::string& method, const util::URL& target,
                          const fields& send, const std::string& body,
                          const util::http::body_sink* sink)
{
    if(!util::http::grammar().at("method").try_parse(method))
        throw error("not an HTTP method: \"" + method + "\"");

    const std::string scheme = target.get_protocol();
    const bool tls = scheme == "https";

    if(scheme != "http" && !tls)
        throw error("not an HTTP URL: \"" + target.coagulate() + "\"");

    const unsigned int port = target.get_port().empty()
                              ? (tls ? 443u : 80u)
                              : target.get_port_val();

    // **The refusal that makes this safe to hold Authorization on.**  A
    // connection is bound to one origin, and a target for another is a
    // mistake rather than a thing to go and do: sending it down this socket
    // would reach the wrong server, and opening a new one to reach the right
    // one would spend the caller's fields somewhere they did not look.
    if(!same_origin(m_origin, target))
        throw error("this connection is to " + origin_text(m_origin) +
                    " and that request is for " + origin_text(target));

    for(const fields::value_type& f : send) check_field(f.first, f.second);

    if(!m_sock || !*m_sock) {
        m_sock = transport(target, port, tls, m_o);

        m_live = true;
        m_served = 0;
    }

    std::ostringstream head;

    head << method << " " << request_target(target) << " HTTP/1.1\r\n";

    // The one field that differs from a one-shot request, and the whole point
    // of this class.  HTTP/1.1 is persistent by default, so saying keep-alive
    // is strictly speaking redundant -- it is sent because a server that
    // cares is likelier to have been written against the explicit form, and
    // because it makes the intent visible in a packet capture.
    fields out;

    out.add("Host", host_field(target, port, tls));
    out.add("User-Agent", m_o.user_agent);
    out.add("Connection", "keep-alive");
    out.add("Accept-Encoding", "identity");

    if(!body.empty()) out.add("Content-Length", std::to_string(body.size()));

    for(const fields::value_type& f : out) {
        if(!send.has(f.first)) head << f.first << ": " << f.second << "\r\n";
    }

    for(const fields::value_type& f : send) {
        head << f.first << ": " << f.second << "\r\n";
    }

    head << "\r\n";

    // What *we* said, which decides as much as what the server says.  A
    // caller can override Connection, and request() does -- so a connection
    // that announced it was closing must not then be reused, whatever the
    // response carries.  Missing this is invisible against a lenient server
    // and a protocol violation against a strict one.
    const bool we_said_close =
        util::http::fold(send.get("Connection")).find("close") !=
        std::string::npos;

    if(debug()) std::cerr << head.str() << std::flush;

    *m_sock << head.str() << body << std::flush;

    if(!*m_sock) {
        close();

        throw error("the connection failed while sending the request");
    }

    Response r;

    // Whether the caller's sink asked to stop, which read_body does not
    // report -- it returns void, and adding a return to it for this one
    // caller would be widening a just-settled interface for a question the
    // caller can answer itself.  This is that answer: watch the sink.
    bool cut = false;

    try {
        r = util::http::read_response_head(*m_sock, method == "HEAD");

        if(sink) {
            util::http::read_body(*m_sock, r.body_framing(), r.content_length(),
                                  [&cut, sink](std::string_view piece) {
                                      if((*sink)(piece)) return true;

                                      cut = true;

                                      return false;
                                  },
                                  util::http::no_cap);
        }
        else {
            r.set_body(util::http::read_body(*m_sock, r, m_o.max_body));
        }
    }
    catch(std::exception&) {
        // Whatever went wrong, this socket is no longer at a message boundary
        // and nothing can put it back there.
        close();

        throw;
    }

    m_served++;

    // A body the sink cut short leaves the stream in the middle of a message,
    // where the next read would take the rest of the body for a status line.
    m_live = !cut && !we_said_close && persists(r, method);

    if(!m_live) m_sock.reset();

    if(debug())
        std::cerr << r.status() << " " << r.reason() << ", request "
                  << m_served << " on this connection, "
                  << (m_live ? "kept" : "closed") << std::endl;

    return r;
}

Response request(const std::string& method,
                 const util::URL& url,
                 const fields& send,
                 const std::string& body,
                 const options& o)
{
    util::URL target = url;
    std::string verb = method;
    std::string payload = body;
    fields carry = send;

    for(unsigned int hop = 0; ; hop++) {
        const Response r = once(verb, target, carry, payload, o);

        if(hop >= o.redirects) return r;

        const bool redirect = r.status() == 301 || r.status() == 302 ||
                              r.status() == 303 || r.status() == 307 ||
                              r.status() == 308;

        if(!redirect || !r.fields().has("Location")) return r;

        // A 303 says "go and GET this instead" and drops the body; 307 and 308
        // say "same request, elsewhere".  RFC 9110 15.4.4.
        if(r.status() == 303) {
            verb = "GET";
            payload.clear();
        }

        // Anything else is not followed, and this is the important half.
        // Re-sending a POST body to a host named by the response is not a
        // redirect, it is handing the request to whoever wrote the Location --
        // and for the request this client exists to make, the body is a
        // client_secret and a refresh_token.
        if(verb != "GET" && verb != "HEAD") return r;

        util::URL next;

        try {
            next.parse_reference(util::trim(r.fields().get("Location")));
        }
        catch(std::exception&) {
            throw error("a redirect to something that is not a URI reference: \"" +
                        r.fields().get("Location") + "\"");
        }

        if(next.relative()) {
            // No RFC 3986 5.3 here, and inventing half of it would be worse
            // than not having it.  An absolute path against the same origin is
            // the case that resolves without an algorithm, and it is the one
            // servers actually send.
            if(next.get_path().empty() || next.get_path()[0] != '/') {
                throw error("a relative redirect that is not an absolute path: \"" +
                            r.fields().get("Location") + "\"");
            }

            util::URL resolved(target);

            resolved.set_path(next.get_path());
            resolved.set_qs(next.get_qs());
            resolved.set_fragment("");

            next = resolved;
        }

        // Origin: scheme, host and port together.  Authorization is scoped to
        // one of those and to no other, so it comes off whenever any of the
        // three changes -- which is what stops a redirect from being a way to
        // read somebody's bearer token.
        const bool same_origin = next.get_protocol() == target.get_protocol() &&
                                 next.get_host() == target.get_host() &&
                                 next.get_port() == target.get_port();

        if(!same_origin) {
            fields kept;

            for(const fields::value_type& f : carry) {
                if(util::http::fold(f.first) != "authorization" &&
                   util::http::fold(f.first) != "cookie")
                    kept.add(f.first, f.second);
            }

            carry = kept;
        }

        // The proxy parameter travels with the request rather than with the
        // URL: a Location: names a resource, not a route to it.
        if(!target["proxy"].empty()) {
            std::map<std::string, std::string> qs = next.get_qs_hash();

            qs["proxy"] = target["proxy"];
            next.set_qs(qs);
        }

        target = next;
    }
}

Response get(const util::URL& url, const fields& send, const options& o) {
    return request("GET", url, send, "", o);
}

Response post_form(const util::URL& url,
                   const std::map<std::string, std::string>& form,
                   const fields& send,
                   const options& o)
{
    fields with = send;

    if(!with.has("Content-Type"))
        with.add("Content-Type", "application/x-www-form-urlencoded");

    return request("POST", url, with, form_encode(form), o);
}

}
}
}
