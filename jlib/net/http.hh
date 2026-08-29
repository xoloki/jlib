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

#include <jlib/util/URL.hh>
#include <jlib/util/http.hh>

#include <map>
#include <string>

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

    std::string user_agent = "jlib/1.2";
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
