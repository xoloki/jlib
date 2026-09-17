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

#ifndef JLIB_NET_HTTP_HH
#define JLIB_NET_HTTP_HH

#include <jlib/sys/socketstream.hh>
#include <jlib/util/URL.hh>
#include <jlib/util/http.hh>

#include <map>
#include <string>
#include <memory>

namespace jlib {
namespace net {

/**
 * An HTTP/1.1 client.
 *
 * ## Narrow, and closed
 *
 * What an OAuth2 token exchange needs and nothing else: GET, POST, a form
 * body, Content-Length and chunked responses, TLS, and a CONNECT proxy.  No
 * cookies, no keep-alive, no compression, no content negotiation, no HTTP/2,
 * no connection pool.
 *
 * That list is here so it stays true.  Nothing in this tree has ever called
 * the old jlib::net::http -- it declared a Request and a Response that were
 * never defined anywhere, and a get() that was HTTP/1.0 and treated any status
 * but 200 as an error -- so there is no existing user pulling this wider, and
 * the way a narrow thing becomes a broad one is by nobody writing down that it
 * was meant to be narrow.
 *
 * ## Two halves in two libraries
 *
 * Reading a response is jlib::util::http, because jlib::sys needs it:
 * basic_proxybuf reads the answer to CONNECT and jlib::sys may not include
 * jlib::net.  The names are pulled through below, so a caller says
 * net::http::Response and gets the same class either way.
 */
namespace http {

using jlib::util::http::error;
using jlib::util::http::fields;
using jlib::util::http::framing;
using jlib::util::http::Request;
using jlib::util::http::Response;

/** How to make a request. */
/** The default User-Agent: "jlib/" and the release.  See options::user_agent. */
std::string default_user_agent();

struct options {
    /**
     * Seconds to allow the connect, and each read afterwards.  Zero waits.
     *
     * A default rather than the library's, which is off: a mail client is
     * supposed to sit on a quiet IMAP connection and an HTTP client is not.
     */
    double timeout = 30;

    /** The most body octets to read.  A token response is a few hundred. */
    std::size_t max_body = 1 << 20;

    /**
     * How many 3xx to follow.  **Zero, and that is not timidity.**
     *
     * Following a redirect means sending the request again to a host named by
     * the response.  For the call this client exists to make that request
     * carries a client_secret and a refresh_token, and a Location: is
     * attacker-controlled the moment anything upstream is.  Turning it on is a
     * decision a caller makes for a particular request, which is why it is
     * here and not a default.
     *
     * Even switched on: only GET and HEAD are followed, because re-sending a
     * POST body somewhere else is not a redirect; a 303 becomes a GET, as RFC
     * 9110 15.4.4 says; and Authorization is dropped whenever the origin
     * changes.
     */
    unsigned int redirects = 0;

    /**
     * What jlib calls itself on the wire.
     *
     * A function rather than a literal, because the literal said "jlib/1.2"
     * for a while after the library became 2.0.0 -- it is in a header that is
     * *installed*, so the version cannot reach it as a macro without reaching
     * every consumer's compile as well.  Defined in http.cc, where it can.
     */
    std::string user_agent = default_user_agent();
};

/**
 * Make one request and read the whole response.
 *
 * @param method  "GET", "POST", ...  Must be a token.
 * @param url     http or https.  The query string is part of the target; the
 *                fragment is not sent, per RFC 9110 7.1.
 * @param send    fields to add.  Host, User-Agent, Connection, Content-Length
 *                and Accept-Encoding are supplied and may be overridden.
 * @param body    the request body, sent with a Content-Length.
 *
 * **A non-2xx is a Response, not an exception.**  An OAuth2 token endpoint
 * answers 400 with a JSON body saying whether the refresh token was revoked or
 * the server merely hiccuped, and a client that throws away the body of
 * anything it does not like cannot tell those apart.  What throws is a failure
 * to complete an exchange at all: no connection, a malformed head, a body that
 * ends early or runs past max_body.
 *
 * Field names and values from a caller are checked against the grammar before
 * they go out.  A CR or LF in a value is how one request becomes two.
 */
Response request(const std::string& method,
                 const jlib::util::URL& url,
                 const fields& send = fields(),
                 const std::string& body = "",
                 const options& o = options());

/**
 * Several requests down one connection, and a body taken as it arrives.
 *
 * `request()` above opens a socket, says `Connection: close`, reads the whole
 * response and hangs up. That is the right shape for the call this client was
 * written for -- one OAuth2 token exchange -- and the wrong one for a caller
 * that makes many: jcode asks a local jserve for a completion per edit, and
 * paid a TCP handshake, and a TLS handshake where there is one, for each (#242).
 *
 * ## What it is not
 *
 * **Not a pool.** One connection to one origin, opened on first use, owned by
 * the caller and closed when it goes. Nothing here picks a connection for you,
 * counts them, or hands them round between threads; a `connection` belongs to
 * one thread the way a `socketstream` does.
 *
 * **Not a redirect follower.** `request()` keeps that, because following one
 * means connecting somewhere else and this object is bound to an origin. A
 * target for a different host is refused rather than sent -- which is the
 * point: a connection carrying an Authorization must not be talked into
 * spending it on a host named by somebody else.
 *
 * ## When it stops being usable
 *
 * `live()` goes false when the response said `Connection: close`, when an
 * HTTP/1.0 response did not ask to stay, when the body was framed by the
 * connection ending, when anything threw, and when a sink stopped a body
 * early -- that last because the stream is then not at a message boundary and
 * the next read would take the rest of a body for a status line. A caller
 * checks `live()` or simply keeps calling: a dead connection reopens on the
 * next request, which is what a pool would have done anyway.
 */
class connection {
public:
    /**
     * @param origin scheme, host and port; the path is ignored
     *
     * Nothing is connected here. The socket opens on the first request, so
     * constructing one costs nothing and a caller may keep one against the
     * possibility of using it.
     */
    connection(const jlib::util::URL& origin, const options& o = options());

    ~connection();

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;

    /**
     * One request; the whole response, as request() gives it.
     *
     * @throws error if the target names a different origin, or the exchange
     *         could not be completed
     */
    Response request(const std::string& method,
                     const jlib::util::URL& target,
                     const fields& send = fields(),
                     const std::string& body = "");

    /**
     * One request; the body handed to `sink` as it arrives.
     *
     * The Response comes back with **no body in it** -- the sink got it. That
     * is deliberate rather than convenient: a caller streaming a body has
     * already said it will not hold the whole thing, and filling `body()` as
     * well would be doing exactly what it asked not to happen.
     *
     * `options::max_body` does not apply. The sink decides when it has had
     * enough by returning false, and see `live()` for what that costs.
     */
    Response request(const std::string& method,
                     const jlib::util::URL& target,
                     const fields& send,
                     const std::string& body,
                     const jlib::util::http::body_sink& sink);

    /** Whether another request may be sent without reconnecting. */
    bool live() const { return m_live; }

    /** How many requests have gone down the socket that is open now. */
    std::size_t served() const { return m_served; }

    /** Hang up.  The next request opens a new socket. */
    void close();

private:
    Response send(const std::string& method, const jlib::util::URL& target,
                  const fields& send, const std::string& body,
                  const jlib::util::http::body_sink* sink);

    jlib::util::URL m_origin;
    options m_o;

    std::unique_ptr<jlib::sys::socketstream> m_sock;

    bool m_live = false;
    std::size_t m_served = 0;
};

Response get(const jlib::util::URL& url,
             const fields& send = fields(),
             const options& o = options());

/**
 * POST an application/x-www-form-urlencoded body.
 *
 * Which is how every OAuth2 token request is made (RFC 6749 4.1.3).
 */
Response post_form(const jlib::util::URL& url,
                   const std::map<std::string, std::string>& form,
                   const fields& send = fields(),
                   const options& o = options());

/**
 * application/x-www-form-urlencoded.
 *
 * **Not uri::encode.**  This encoding is HTML's, not RFC 3986's: a space is
 * "+" here and "%20" there, and "+" itself therefore has to be escaped.  A
 * token containing a "+" -- base64 produces them constantly -- sent through
 * the wrong encoder arrives as a space and does not match.
 */
std::string form_encode(const std::map<std::string, std::string>& form);

}
}
}

#endif // JLIB_NET_HTTP_HH
