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

#ifndef JLIB_UTIL_HTTP_HH
#define JLIB_UTIL_HTTP_HH

#include <jlib/util/abnf.hh>

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jlib {
namespace util {

/**
 * Reading an HTTP/1.1 response, against RFC 9112's grammar.
 *
 * The message layer only: a status line, a field section, and how the body
 * that follows them is delimited.  Making the request and choosing a transport
 * is a client's job and lives above this.
 *
 * ## Why this is in util and not net
 *
 * Because sys/proxystream.hh needs it.  basic_proxybuf::open_proxy() reads an
 * HTTP response head -- the answer to CONNECT -- and it was the only correct
 * head reader in the tree: it capped the length, it insisted on a status line,
 * and it read to the blank line rather than throwing the proxy's own headers
 * into the protocol stream.  Having a second one in net would leave two, which
 * is what this branch exists to avoid.  jlib::sys is below jlib::net and may
 * not include it; it already includes jlib/util/util.hh, so this is the same
 * seam rather than a new one.
 *
 * It is also where the other RFC readers live -- URL for 3986, content_type
 * for 2045, encoded_word for 2047 -- and rfc9110.hh and rfc9112.hh compose
 * onto rfc3986.hh, which is next door.
 *
 * ## What this deliberately does not do
 *
 * No cookies, no keep-alive, no content negotiation, no compression, no
 * HTTP/2.  The client this serves fetches an OAuth2 token, and the way a
 * narrow thing becomes a broad one is by nobody writing down that it was meant
 * to be narrow.
 */
namespace http {

class error : public std::exception {
public:
    error(const std::string& msg = "") { m_msg = "http error: " + msg; }
    virtual ~error() {}
    virtual const char* what() const noexcept { return m_msg.c_str(); }
protected:
    std::string m_msg;
};

/**
 * A field section: names to values, in the order they arrived.
 *
 * **Not util::Headers**, and that is a decision rather than an oversight.
 * Headers is an RFC 5322 header set: it RFC 2047-decodes every value it is
 * given, unconditionally, folds continuation lines, and compares names with
 * case.  HTTP field names are case-insensitive (RFC 9110 5.1), HTTP has no
 * encoded words at all, and an HTTP field value that happens to contain
 * "=?utf-8?q?..." means those characters and nothing else.  Handing one to the
 * other would decode a value nobody asked to have decoded.
 *
 * A field may appear more than once and the repeats are not interchangeable
 * -- two Content-Lengths that disagree is an attack, not a formatting choice
 * -- so nothing here collapses them.
 */
class fields {
public:
    typedef std::pair<std::string, std::string> value_type;
    typedef std::vector<value_type> rep_type;
    typedef rep_type::const_iterator const_iterator;
    typedef rep_type::size_type size_type;

    void add(std::string name, std::string value);

    /** Case-insensitively, per RFC 9110 5.1. */
    bool has(std::string_view name) const;

    /** The first value, or "" -- which is also what an empty field gives. */
    std::string get(std::string_view name) const;

    /** Every value under that name, in the order received. */
    std::vector<std::string> all(std::string_view name) const;

    /** How many times that name appears. */
    size_type count(std::string_view name) const;

    const_iterator begin() const { return m_fields.begin(); }
    const_iterator end() const { return m_fields.end(); }
    size_type size() const { return m_fields.size(); }
    bool empty() const { return m_fields.empty(); }

private:
    rep_type m_fields;
};

/** How the body after a response head is delimited.  RFC 9112 6.3. */
enum class framing {
    none,          ///< there is no body: 1xx, 204, 304, or a response to HEAD
    length,        ///< Content-Length octets
    chunked,       ///< Transfer-Encoding ends in chunked
    until_close    ///< to end of stream, which is the last resort
};

/** One response: its head, and its body once something has read one. */
class Response {
public:
    Response() = default;

    /** The three-digit code as a number.  Not a string to be compared. */
    int status() const { return m_status; }

    /** "HTTP/1.1". */
    const std::string& version() const { return m_version; }

    /** The reason phrase, which may be empty and means nothing to a program. */
    const std::string& reason() const { return m_reason; }

    const http::fields& fields() const { return m_fields; }

    const std::string& body() const { return m_body; }
    void set_body(std::string body) { m_body = std::move(body); }

    /**
     * 2xx.
     *
     * A non-2xx is not an exception here and must not become one.  An OAuth2
     * token endpoint answers 400 with a JSON body that says whether the
     * refresh token was revoked or the server merely hiccuped, and a client
     * that throws away the body of anything it does not like cannot tell those
     * apart.
     */
    bool ok() const { return m_status >= 200 && m_status < 300; }

    http::framing body_framing() const { return m_framing; }

    /** Meaningful only when body_framing() is framing::length. */
    std::size_t content_length() const { return m_length; }

    friend Response parse_head(std::string_view head, bool head_request);

private:
    int m_status = 0;
    std::string m_version;
    std::string m_reason;
    http::fields m_fields;
    std::string m_body;
    http::framing m_framing = framing::until_close;
    std::size_t m_length = 0;
};

/**
 * One request: its head, and its body once something has read one.
 *
 * The mirror of Response, and here rather than in net for the same reason: the
 * grammar, the line splitting and the framing checks are all in this file and
 * must not be written twice.  net::http re-exports the name, so a caller says
 * net::http::Request either way.
 */
class Request {
public:
    Request() = default;

    /** "GET".  A token, and compared with case -- RFC 9110 9.1. */
    const std::string& method() const { return m_method; }

    /** The request-target exactly as it arrived, query and all. */
    const std::string& target() const { return m_target; }

    const std::string& version() const { return m_version; }

    const http::fields& fields() const { return m_fields; }

    const std::string& body() const { return m_body; }
    void set_body(std::string body) { m_body = std::move(body); }

    http::framing body_framing() const { return m_framing; }

    /** Meaningful only when body_framing() is framing::length. */
    std::size_t content_length() const { return m_length; }

    /** The Host field, which HTTP/1.1 requires -- RFC 9112 3.2. */
    std::string host() const { return m_fields.get("Host"); }

    friend Request parse_request_head(std::string_view head);

private:
    std::string m_method;
    std::string m_target;
    std::string m_version;
    http::fields m_fields;
    std::string m_body;
    http::framing m_framing = framing::none;
    std::size_t m_length = 0;
};

/**
 * Parse a request head against the grammar.
 *
 * The same refusals parse_head() makes -- both Content-Length and
 * Transfer-Encoding, two Content-Lengths that disagree, a Content-Length that
 * is not digits, obs-fold, a bare CR or LF, whitespace before a colon.
 *
 * **One difference from a response, and it matters.**  A request with neither
 * framing field has *no body* (RFC 9112 6.3), where a response with neither
 * runs until the connection closes.  framing::until_close on a request would
 * mean reading until a client that is waiting for an answer gives up -- a hang,
 * not a body.  So the default here is framing::none, and a Transfer-Encoding
 * that does not end in chunked is refused rather than quietly becoming a read
 * to end of stream.
 */
Request parse_request_head(std::string_view head);

/** read_head() then parse_request_head(). */
Request read_request_head(std::istream& is, std::size_t cap = 8192);

/**
 * Read a body whose framing is already known.
 *
 * The Response overload below forwards to this; a Request needs it because its
 * framing is decided by different rules.
 */
std::string read_body(std::istream& is, framing how, std::size_t length,
                      std::size_t cap = 1 << 20);

/**
 * The composed grammar: RFC 3986, then RFC 9110, then RFC 9112.
 *
 * Built once, on first use.  Exposed so a test can ask it what it knows --
 * which rules are still prose, whether it passes check() -- without going
 * through a parse.
 */
const abnf::grammar& grammar();

/**
 * Read a message head from a stream, to and including the blank line.
 *
 * A response head or a request head: the two are the same shape above the
 * start line, and this reads the shape.
 *
 * Returns everything read, CRLFs and all.  Framing is procedural here for the
 * reason written down in rfc9112.hh: abnf::rule::parse takes a string_view, so
 * a grammar cannot ask for more input.  imap::read() frames an IMAP literal
 * the same way.
 *
 * @param cap  the most octets to read before giving up.  A head with no end
 *             to it is otherwise an unbounded read from a stranger.
 *
 * @throw error if the stream ends first, or the cap is reached.
 */
std::string read_head(std::istream& is, std::size_t cap = 8192);

/**
 * Parse a response head against the grammar.
 *
 * @param head_request  true if this answers a HEAD, which has no body however
 *                      the fields are written (RFC 9112 6.3)
 *
 * Refuses, per RFC 9112 6:
 *
 * - both Content-Length and Transfer-Encoding, which is the request-smuggling
 *   primitive: two recipients disagreeing about where a message ends is how
 *   one request becomes two;
 * - two Content-Lengths that do not agree, which is the same thing spelled
 *   differently;
 * - a Content-Length that is not a run of digits;
 * - obs-fold, the continuation line, which 9112 5.2 says a recipient must
 *   reject or replace;
 * - whitespace before the colon, and a bare CR or LF anywhere in the section,
 *   both of which the grammar refuses on its own.
 */
Response parse_head(std::string_view head, bool head_request = false);

/** read_head() then parse_head(). */
Response read_response_head(std::istream& is, bool head_request = false,
                            std::size_t cap = 8192);

/**
 * Read a body, however the head says it is delimited.
 *
 * @param cap  the most octets to keep.  A Content-Length arriving from a
 *             stranger decides an allocation; a token response is a few
 *             hundred octets and anything claiming a gigabyte is not one.
 *
 * Chunked is read the size line, read that many octets, eat the CRLF, repeat
 * -- and the trailer section after the last chunk is read and discarded, since
 * something has to consume it for the connection to be at a boundary.
 */
std::string read_body(std::istream& is, const Response& head,
                      std::size_t cap = 1 << 20);

/** Lower-cased ASCII, for comparing a field name or a token. */
std::string fold(std::string_view s);

}
}
}

#endif // JLIB_UTIL_HTTP_HH
