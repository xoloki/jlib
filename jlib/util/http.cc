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

#include <jlib/util/http.hh>

#include <coroutine>

#include <jlib/util/rfc3986.hh>
#include <jlib/util/rfc9110.hh>
#include <jlib/util/rfc9112.hh>

#include <algorithm>
#include <cctype>
#include <istream>
#include <sstream>

namespace jlib {
namespace util {
namespace http {

std::string fold(std::string_view s) {
    std::string out;

    out.reserve(s.size());

    for(char c : s) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return out;
}

const abnf::grammar& grammar() {
    // On first use, not at namespace scope: a grammar built while the library
    // loads runs before main(), which is the mistake crypt/curve.hh documents
    // at length.  content_type.cc builds its grammar the same way.
    static const abnf::grammar g = [] {
        abnf::grammar built = abnf::compile(std::string(rfc3986::URI_GRAMMAR) +
                                            rfc9110::SEMANTICS +
                                            rfc9112::MESSAGING);

        built.check();

        return built;
    }();

    return g;
}

// ------------------------------------------------------------------- fields

void fields::add(std::string name, std::string value) {
    m_fields.emplace_back(std::move(name), std::move(value));
}

bool fields::has(std::string_view name) const {
    return count(name) != 0;
}

std::string fields::get(std::string_view name) const {
    const std::string want = fold(name);

    for(const value_type& f : m_fields) {
        if(fold(f.first) == want) return f.second;
    }

    return std::string();
}

std::vector<std::string> fields::all(std::string_view name) const {
    const std::string want = fold(name);
    std::vector<std::string> out;

    for(const value_type& f : m_fields) {
        if(fold(f.first) == want) out.push_back(f.second);
    }

    return out;
}

fields::size_type fields::count(std::string_view name) const {
    const std::string want = fold(name);
    size_type n = 0;

    for(const value_type& f : m_fields) {
        if(fold(f.first) == want) n++;
    }

    return n;
}

// ---------------------------------------------------------------- read_head

namespace {

/**
 * Whether the head accumulated so far has reached its blank line.
 *
 * Shared by the synchronous read_head and the coroutine one, because it is the
 * half most likely to drift: two copies of a termination test is two places to
 * get "\r\n\r\n" wrong and one place to notice.
 *
 * One octet at a time, and not getline: a bare LF in the middle of the section
 * is something the caller has to be able to see and refuse, and getline would
 * silently make it a line ending.
 *
 * "\n\n" ends it as well as "\r\n\r\n" only so that a head sent with bare LFs
 * is *read* and then *rejected* by the grammar, with a message saying what was
 * wrong, rather than read until the cap runs out.
 */
bool head_complete(const std::string& head) {
    return head.size() >= 4 &&
        (head.compare(head.size() - 4, 4, "\r\n\r\n") == 0 ||
         head.compare(head.size() - 2, 2, "\n\n") == 0);
}

/** The cap, which both read the same way. */
void refuse_if_too_long(const std::string& head, std::size_t cap) {
    if(head.size() > cap) {
        throw error("no end to the message head within " +
                    std::to_string(cap) + " octets");
    }
}

std::string ended_early(const std::string& head) {
    return "the connection closed before the message head ended, after " +
        std::to_string(head.size()) + " octets";
}

}

std::string read_head(std::istream& is, std::size_t cap) {
    std::string head;

    while(!head_complete(head)) {
        const int c = is.get();

        if(c == std::char_traits<char>::eof()) throw error(ended_early(head));

        head += static_cast<char>(c);

        refuse_if_too_long(head, cap);
    }

    return head;
}

/**
 * The suspending framing loop, which both suspending reads of a head share.
 *
 * **The first framing function to change shape**, and the point of it is how
 * little changed: the loop is the loop above, `is.get()` became `in.get()`, and
 * the branch that threw on end of input became "top up, and throw only if
 * there is nothing more coming".
 *
 * What did *not* change is everything downstream.  parse_request_head and
 * parse_head take a string_view and are untouched, because the framing and the
 * parsing were already separate -- a split the RFC-grammar work made for
 * unrelated reasons and which is why #4 costs six functions rather than
 * fifty-nine.
 */
sys::task<bool> read_head_if_any(sys::async_reader& in, std::size_t cap,
                                 std::string& head)
{
    head.clear();

    while(!head_complete(head)) {
        const int c = in.get();

        if(c == sys::async_reader::empty) {
            if(!co_await in.fill()) {
                // Nothing arrived at all: the peer is done, and said so the
                // way HTTP says it.  Anything else is a message cut in half.
                if(head.empty()) co_return false;

                throw error(ended_early(head));
            }

            continue;
        }

        head += static_cast<char>(c);

        refuse_if_too_long(head, cap);
    }

    co_return true;
}

/**
 * The same, for a caller to whom a connection that ends is always an error.
 *
 * Every suspending read of a head goes through read_head_if_any above; this
 * adds the throw, and that is the entire difference between the two.
 *
 * A caller that does not want to be a coroutine drives this with
 * sys::run_until_complete; see the note on it.
 */
sys::task<std::string> read_head(sys::async_reader& in, std::size_t cap) {
    std::string head;

    // The loop moved to read_head_if_any, which this is the strict half of:
    // the two differ only in whether a connection that ended before a head
    // started is an error, and keeping two copies of a framing loop is how the
    // two ends of a protocol come to disagree about where a message stops.
    if(!co_await read_head_if_any(in, cap, head)) throw error(ended_early(head));

    co_return head;
}

// --------------------------------------------------------------- parse_head

namespace {

    /**
     * Split a head into lines on CRLF, refusing anything else.
     *
     * RFC 9112 2.2 lets a recipient accept a bare LF as a line terminator and
     * says it MUST NOT accept a bare CR.  jlib accepts neither, because the
     * whole reason to care is that a recipient which disagrees with the one in
     * front of it about where a line ends is how one message becomes two --
     * and the only way to disagree with nobody is to insist on CRLF.
     *
     * **`what` names which head is being parsed**, and it is not decoration.
     * This is called for requests and for responses, and it used to say
     * "response" either way -- so a scanner sending a malformed request line
     * produced, in a server's error log:
     *
     *     [client 152.32.183.27:52808] the response head does not end in CRLF
     *
     * which tells the operator their own server emitted something broken. It
     * was the second error line the six sites running this logged after a
     * deploy whose entire point was making that log trustworthy.
     */
    std::vector<std::string> split_lines(std::string_view head,
                                         const char* what) {
        std::vector<std::string> lines;
        std::size_t i = 0;

        while(i < head.size()) {
            const std::size_t nl = head.find("\r\n", i);

            if(nl == std::string_view::npos)
                throw error(std::string("the ") + what +
                            " head does not end in CRLF");

            const std::string_view line = head.substr(i, nl - i);

            if(line.find('\r') != std::string_view::npos ||
               line.find('\n') != std::string_view::npos) {
                throw error(std::string("a bare CR or LF inside the ") +
                            what + " head");
            }

            if(line.empty()) {
                // The blank line.  Anything after it is the body's business.
                return lines;
            }

            lines.emplace_back(line);
            i = nl + 2;
        }

        throw error(std::string("the ") + what +
                    " head has no blank line at the end of it");
    }

    bool all_digits(const std::string& s) {
        if(s.empty()) return false;

        for(char c : s) {
            if(c < '0' || c > '9') return false;
        }

        return true;
    }

}

namespace {

    /**
     * The field lines of a head, read against field-line.
     *
     * Shared by a request and a response because a field section is the same
     * production in both -- RFC 9112 2.1 puts start-line above it and says
     * nothing else differs.
     */
    /**
     * How many fields one message may carry.
     *
     * The head's *size* is already capped by the caller -- 8192 octets by
     * default in net::http::server -- and for a while that was taken to bound
     * this too. It does not bound it well: several hundred short fields fit
     * inside 8 KB, and `fields` looks up linearly while the server does
     * several lookups per request, so the work grows with the count.
     *
     * Measured during #240, best of five, against 0.17 ms for a plain request:
     *
     *     50 fields (419 octets)    0.36 ms
     *     100       (819)           0.57
     *     200       (1719)          0.97
     *     400       (3519)          1.70
     *     700       (6219)          2.57
     *
     * Two things worth saying about those numbers. The growth is **linear**,
     * not quadratic as #240 guessed -- so this is a sharp edge rather than a
     * hole, and anyone who went looking for the quadratic would have found
     * nothing and concluded there was no problem. And the amplification is
     * real anyway: roughly 15x for a request costing the attacker 6 KB.
     *
     * 100 is **Apache's `LimitRequestFields` default**, which is a better
     * justification than a number invented here: it is the most widely
     * deployed answer to this exact question and nothing has broken on it. A
     * browser sends on the order of twenty.
     *
     * Deliberately not a parameter. `parse_head` is public with four
     * overloads and threading a cap through all of them for a bound no caller
     * has ever wanted to raise is a worse trade than a constant with its
     * reasoning attached.
     */
    const std::size_t MAX_FIELDS = 100;

    void read_fields(const std::vector<std::string>& lines, std::size_t from,
                     fields& into)
    {
        const abnf::rule field = grammar().at("field-line");

        if(lines.size() > from && lines.size() - from > MAX_FIELDS) {
            throw error("too many header fields: " +
                        std::to_string(lines.size() - from) + ", and " +
                        std::to_string(MAX_FIELDS) + " is the limit");
        }

        for(std::size_t i = from; i < lines.size(); i++) {
            // obs-fold: a line beginning with SP or HTAB continues the one
            // before it.  RFC 9112 5.2 says a recipient of a message that is
            // not a message/http payload must either reject it or replace the
            // fold with a space; rejecting is the safe half of that choice.
            if(lines[i][0] == ' ' || lines[i][0] == '\t') {
                throw error("obs-fold, an obsolete line continuation, in the "
                            "message head: \"" + lines[i] + "\"");
            }

            const abnf::parse_result p = field.try_parse(lines[i]);

            if(!p) throw error("not a header field: \"" + lines[i] + "\"");

            const abnf::match m = p.root();

            // The value comes from the match, not from find(':'), which is the
            // point of having the grammar: field-line puts the OWS outside
            // field-value, so what is captured is already trimmed at both ends.
            into.add(m["field-name"].str(), m["field-value"].str());
        }
    }

    /**
     * RFC 9112 6.  Which of Content-Length and Transfer-Encoding governs, and
     * what to refuse outright.
     *
     * @param no_body   this message cannot have one whatever the fields say
     * @param bodyless  what to use when neither field is present: framing::none
     *                  for a request, framing::until_close for a response
     */
    void decide_framing(const fields& f, bool no_body, framing bodyless,
                        framing& how, std::size_t& length)
    {
        const bool has_te = f.has("Transfer-Encoding");
        const bool has_cl = f.has("Content-Length");

        how = bodyless;
        length = 0;

        if(has_te && has_cl) {
            // The request-smuggling primitive.  RFC 9112 6.1: a message with
            // both "ought to be handled as an error"; the reason it is an error
            // rather than a preference is that a proxy in front of us may pick
            // the other one, and then the tail of this message is the head of
            // the next request as far as one of us is concerned.
            throw error("both Content-Length and Transfer-Encoding are present; "
                        "where this message ends depends on who is reading it");
        }

        if(has_cl) {
            const std::vector<std::string> all = f.all("Content-Length");

            for(const std::string& v : all) {
                if(!all_digits(v))
                    throw error("Content-Length is not a number: \"" + v + "\"");

                if(v != all[0])
                    throw error("two Content-Length fields that do not agree: \"" +
                                all[0] + "\" and \"" + v + "\"");
            }

            try {
                length = static_cast<std::size_t>(std::stoull(all[0]));
            }
            catch(std::exception&) {
                throw error("Content-Length does not fit: \"" + all[0] + "\"");
            }

            how = framing::length;
        }
        else if(has_te) {
            // 6.1: chunked must be the final coding.
            const std::string te = fold(f.get("Transfer-Encoding"));
            const std::size_t last = te.find_last_of(',');
            const std::string final = last == std::string::npos
                                      ? te : te.substr(last + 1);

            std::string trimmed;

            for(char c : final) {
                if(c != ' ' && c != '\t') trimmed += c;
            }

            if(trimmed == "chunked") {
                how = framing::chunked;
            }
            else if(bodyless == framing::none) {
                // A request.  6.3: if the final coding is not chunked the
                // server cannot tell where the body ends, and reading to close
                // would mean waiting out a client that is waiting for us.
                throw error("a request whose Transfer-Encoding does not end in "
                            "chunked has no discernible end: \"" +
                            f.get("Transfer-Encoding") + "\"");
            }
            else {
                how = framing::until_close;
            }
        }

        if(no_body) {
            how = framing::none;
            length = 0;
        }
    }

}

bool connection_close(const fields& f) {
    for(const std::string& v : f.all("Connection")) {
        std::string token;

        // One pass, splitting on commas and dropping the optional whitespace
        // around each token.  fold() lower-cases but does not trim, which is
        // why the spaces come out here rather than there.
        for(std::size_t i = 0; i <= v.size(); ++i) {
            if(i == v.size() || v[i] == ',') {
                if(fold(token) == "close") return true;

                token.clear();
            }
            else if(v[i] != ' ' && v[i] != '\t') {
                token += v[i];
            }
        }
    }

    return false;
}

bool persistent(const Request& q) {
    if(q.version() != "HTTP/1.1") return false;

    return !connection_close(q.fields());
}

Request parse_request_head(std::string_view head) {
    const std::vector<std::string> lines = split_lines(head, "request");

    if(lines.empty()) throw error("the request head is empty");

    Request r;

    {
        const abnf::parse_result p = grammar().at("request-line").try_parse(lines[0]);

        if(!p) throw error("not a request line: \"" + lines[0] + "\"");

        const abnf::match m = p.root();

        r.m_method = m["method"].str();
        r.m_target = m["request-target"].str();
        r.m_version = m["HTTP-version"].str();
    }

    read_fields(lines, 1, r.m_fields);

    // framing::none when neither field is there, which is the whole difference
    // from a response: RFC 9112 6.3, and see the note in the header.
    decide_framing(r.m_fields, false, framing::none, r.m_framing, r.m_length);

    return r;
}

Request read_request_head(std::istream& is, std::size_t cap) {
    return parse_request_head(read_head(is, cap));
}

bool read_request_head_if_any(std::istream& is, Request& into,
                              std::size_t cap)
{
    std::string head;

    while(!head_complete(head)) {
        const int c = is.get();

        if(c == std::char_traits<char>::eof()) {
            // Nothing arrived at all: the peer is done, and said so the way
            // HTTP says it.  Anything else is a message cut in half.
            if(head.empty()) return false;

            throw error(ended_early(head));
        }

        head += static_cast<char>(c);

        refuse_if_too_long(head, cap);
    }

    into = parse_request_head(head);

    return true;
}

/** The same, suspending.  read_head() then the same parser, unchanged. */
sys::task<Request> read_request_head(sys::async_reader& in, std::size_t cap) {
    co_return parse_request_head(co_await read_head(in, cap));
}

Response parse_head(std::string_view head, bool head_request) {
    const std::vector<std::string> lines = split_lines(head, "response");

    if(lines.empty()) throw error("the response head is empty");

    Response r;

    {
        const abnf::rule status = grammar().at("status-line");

        abnf::parse_result p = status.try_parse(lines[0]);

        if(!p) {
            // RFC 9112 4 writes status-line = HTTP-version SP status-code SP
            // [ reason-phrase ] -- the second SP is required even when the
            // reason is empty.  Servers do omit it.  This is a deliberate
            // leniency and it is narrow: only a line that is otherwise exactly
            // a status line, with the trailing SP supplied.
            p = status.try_parse(std::string(lines[0]) + " ");

            if(!p) throw error("not a status line: \"" + lines[0] + "\"");
        }

        const abnf::match m = p.root();

        r.m_version = m["HTTP-version"].str();
        r.m_status = std::stoi(m["status-code"].str());

        // **A three-digit code is not the same as a status code** (#268).
        //
        // RFC 9112 4 writes status-code = 3DIGIT, so "600" and "999" parse.
        // RFC 9110 15 defines the classes 1xx through 5xx and requires a
        // client to understand a response by the class its first digit names,
        // treating an unrecognised code as the x00 of that class. A 6xx has
        // no class, so there is no such fallback and nothing a caller may do
        // with it -- and everything downstream here switches on status or on
        // status / 100.
        //
        // Found by the coverage-guided fuzzer, from `HTTP/1.1 600 OK`.
        if(r.m_status < 100 || r.m_status > 599) {
            throw error("a status code outside 100..599: \"" +
                        std::string(m["status-code"].str()) + "\"");
        }

        const abnf::match reason = m["reason-phrase"];

        if(reason) r.m_reason = reason.str();
    }

    read_fields(lines, 1, r.m_fields);

    // RFC 9112 6.3.  A response with neither framing field runs until the
    // connection closes, which is the difference from a request.
    const bool no_body = head_request ||
                         (r.m_status >= 100 && r.m_status < 200) ||
                         r.m_status == 204 ||
                         r.m_status == 304;

    decide_framing(r.m_fields, no_body, framing::until_close,
                   r.m_framing, r.m_length);

    return r;
}

Response read_response_head(std::istream& is, bool head_request, std::size_t cap) {
    return parse_head(read_head(is, cap), head_request);
}

/** The same, suspending.  read_head() then the same parser, unchanged. */
sys::task<Response> read_response_head(sys::async_reader& in,
                                       bool head_request, std::size_t cap) {
    co_return parse_head(co_await read_head(in, cap), head_request);
}

// --------------------------------------------------------------- read_body

namespace {

    /**
     * The decisions both read_bodys have to make the same way.
     *
     * Framing is where a difference between the two would be invisible in a
     * test that only reads well-formed bodies and catastrophic against a real
     * peer, so anything that decides *what is wrong* lives here once.  The
     * loops differ, because one blocks and one suspends; nothing else does.
     */

    /** The chunk size, extension parsed off and discarded per RFC 9112 7.1.1. */
    std::size_t chunk_size_of(const std::string& line) {
        const std::size_t semi = line.find(';');
        const std::string size_text = line.substr(0, semi);

        if(!grammar().at("chunk-size").try_parse(size_text))
            throw error("not a chunk size: \"" + size_text + "\"");

        // **`chunk-size = 1*HEXDIG` says any number of digits** (#354), so
        // the grammar check above passes for a line of twenty f's and stoull
        // then throws std::out_of_range. Content-Length has carried this
        // guard since it was written -- the same hazard in the other framing
        // went without one.
        //
        // What escaped was an exception no caller is told about: read_body
        // documents http::error and nothing else, the server's framing catch
        // names that type, and the connection was dropped by the outermost
        // handler where a 400 was intended. Found by the body-reader fuzz
        // target in under thirty seconds, from `fffff...\r\nx\r\n0`.
        try {
            return std::stoull(size_text, 0, 16);
        }
        catch(const std::exception&) {
            throw error("a chunk size that does not fit: \"" + size_text +
                        "\"");
        }
    }

    void refuse_oversized_chunk(std::size_t so_far, std::size_t n,
                                std::size_t cap)
    {
        if(so_far + n > cap) {
            throw error("a chunked body is larger than the " +
                        std::to_string(cap) + " octets that will be read");
        }
    }

    void refuse_oversized_claim(std::size_t n, std::size_t cap) {
        if(n > cap) {
            throw error("the body claims " + std::to_string(n) +
                        " octets and the most that will be read is " +
                        std::to_string(cap));
        }
    }

    void refuse_oversized_stream(std::size_t so_far, std::size_t cap) {
        if(so_far > cap) {
            throw error("a body read to end of stream passed the " +
                        std::to_string(cap) + " octets that will be read");
        }
    }

    std::string short_body(std::size_t missing) {
        return "the connection closed " + std::to_string(missing) +
            " octets short of the body it promised";
    }

    /**
     * How much body is handed over at a time.
     *
     * The same 4096 the until_close loop already read in.  It bounds what a
     * sink is called with, not what it may be called with in total -- the
     * point of a sink is that the total need not fit in memory.
     */
    const std::size_t BODY_BLOCK = 4096;

    /**
     * n octets, to the sink, a block at a time.
     *
     * @return false if the sink asked to stop, in which case the stream is
     *         **not** at a message boundary and the connection cannot be
     *         reused.  That is the caller's to know; nothing here can put the
     *         octets back.
     */
    bool pour(std::istream& is, std::size_t n, const body_sink& sink) {
        char buf[BODY_BLOCK];

        std::size_t left = n;

        while(left) {
            const std::size_t want = left < sizeof buf ? left : sizeof buf;

            is.read(buf, static_cast<std::streamsize>(want));

            const std::size_t got = static_cast<std::size_t>(is.gcount());

            if(got != want) throw error(short_body(left - got));

            left -= got;

            if(!sink(std::string_view(buf, got))) return false;
        }

        return true;
    }

    std::string read_exactly(std::istream& is, std::size_t n, std::size_t cap) {
        refuse_oversized_claim(n, cap);

        std::string out(n, '\0');

        is.read(&out[0], static_cast<std::streamsize>(n));

        if(static_cast<std::size_t>(is.gcount()) != n)
            throw error(short_body(n - std::size_t(is.gcount())));

        return out;
    }

    void eat_crlf(std::istream& is) {
        const int a = is.get();
        const int b = is.get();

        if(a != '\r' || b != '\n')
            throw error("a chunk is not followed by CRLF");
    }

    std::string read_line(std::istream& is, std::size_t cap) {
        std::string line;

        for(;;) {
            const int c = is.get();

            if(c == std::char_traits<char>::eof())
                throw error("the connection closed in the middle of a chunked body");

            if(c == '\n') {
                if(line.empty() || line.back() != '\r')
                    throw error("a bare LF in a chunked body");

                line.pop_back();

                return line;
            }

            line += static_cast<char>(c);

            if(line.size() > cap)
                throw error("a chunked body's line has no end to it");
        }
    }

    // ---- the same three, suspending ----------------------------------------
    //
    // Each is the loop above with the blocking pull replaced by "take what is
    // buffered; when it runs out, co_await a top-up".  Everything that decides
    // what is *wrong* is shared with the blocking versions above.

    sys::task<std::string> read_exactly(sys::async_reader& in, std::size_t n,
                                        std::size_t cap)
    {
        refuse_oversized_claim(n, cap);

        std::string out(n, '\0');

        std::size_t got = 0;

        while(got < n) {
            // In bulk.  get() in a loop would be a call per octet, and a body
            // of a stated length is exactly where that is worth avoiding.
            got += in.take(&out[got], n - got);

            if(got == n) break;

            if(!co_await in.fill()) throw error(short_body(n - got));
        }

        co_return out;
    }

    /**
     * The same, suspending -- and the sink may suspend too.  See the header.
     *
     * The sink is a reference here and **by value one frame up**, in
     * read_body, which awaits this: that frame is alive for the whole of this
     * one, so the reference cannot dangle.  Getting that wrong the other way
     * round is what the note on read_body is about.
     */
    sys::task<bool> pour(sys::async_reader& in, std::size_t n,
                         const async_body_sink& sink)
    {
        char buf[BODY_BLOCK];

        std::size_t left = n;

        while(left) {
            const std::size_t want = left < sizeof buf ? left : sizeof buf;

            const std::size_t took = in.take(buf, want);

            if(took == 0) {
                if(!co_await in.fill()) throw error(short_body(left));

                continue;
            }

            left -= took;

            if(!co_await sink(std::string_view(buf, took))) co_return false;
        }

        co_return true;
    }

    sys::task<void> eat_crlf(sys::async_reader& in) {
        char pair[2];

        std::size_t got = 0;

        while(got < 2) {
            got += in.take(pair + got, 2 - got);

            if(got == 2) break;

            if(!co_await in.fill())
                throw error("a chunk is not followed by CRLF");
        }

        if(pair[0] != '\r' || pair[1] != '\n')
            throw error("a chunk is not followed by CRLF");

        co_return;
    }

    sys::task<std::string> read_line(sys::async_reader& in, std::size_t cap) {
        std::string line;

        for(;;) {
            const int c = in.get();

            if(c == sys::async_reader::empty) {
                if(!co_await in.fill())
                    throw error("the connection closed in the middle of a "
                                "chunked body");

                continue;
            }

            if(c == '\n') {
                if(line.empty() || line.back() != '\r')
                    throw error("a bare LF in a chunked body");

                line.pop_back();

                co_return line;
            }

            line += static_cast<char>(c);

            if(line.size() > cap)
                throw error("a chunked body's line has no end to it");
        }
    }

}

std::string read_body(std::istream& is, const Response& head, std::size_t cap) {
    return read_body(is, head.body_framing(), head.content_length(), cap);
}

void read_body(std::istream& is, framing how, std::size_t length,
               const body_sink& sink, std::size_t cap)
{
    switch(how) {
    case framing::none:
        return;

    case framing::length:
        // Refused before a single octet is read, which is the whole value of
        // a declared length -- and why the cap did not move into the wrapper
        // below.  A body claiming a gigabyte against a megabyte budget costs
        // nothing to refuse here and a megabyte to refuse a block at a time.
        refuse_oversized_claim(length, cap);

        pour(is, length, sink);

        return;

    case framing::chunked: {
        std::size_t so_far = 0;

        for(;;) {
            // chunk-size [ chunk-ext ] CRLF -- the extension is parsed off and
            // thrown away, which is what RFC 9112 7.1.1 says to do with one
            // that is not recognised, and none is.
            const std::size_t n = chunk_size_of(read_line(is, 4096));

            if(n == 0) {
                // The trailer section, read and discarded.  Something has to
                // consume it or the connection is not at a message boundary,
                // and a caller reusing it reads a trailer as a status line.
                for(;;) {
                    const std::string trailer = read_line(is, 4096);

                    if(trailer.empty()) break;
                }

                return;
            }

            // Against the declared size, before reading it, as it always was.
            refuse_oversized_chunk(so_far, n, cap);

            so_far += n;

            if(!pour(is, n, sink)) return;

            eat_crlf(is);
        }
    }

    case framing::until_close: {
        char buf[BODY_BLOCK];

        std::size_t so_far = 0;

        while(is.read(buf, sizeof buf) || is.gcount() > 0) {
            const std::size_t got = static_cast<std::size_t>(is.gcount());

            refuse_oversized_stream(so_far + got, cap);

            so_far += got;

            if(!sink(std::string_view(buf, got))) return;
        }

        return;
    }
    }
}

std::string read_body(std::istream& is, framing how, std::size_t length,
                      std::size_t cap)
{
    std::string body;

    read_body(is, how, length,
              [&body](std::string_view piece) {
                  body.append(piece);

                  return true;
              },
              cap);

    return body;
}

/**
 * The same, suspending instead of blocking.
 *
 * **The framing function the design actually had to survive.**  read_head was
 * a byte loop over a delimiter with nothing carried across a suspension; this
 * has four cases, and the chunked one interleaves *parsing* a chunk size with
 * *reading* chunk data, round after round, with a suspension possible at every
 * step.
 *
 * It survived unchanged in shape: each `co_await` sits exactly where the
 * blocking version made a call that could block, and the control flow is the
 * control flow above.
 *
 * What that costs is **3.27 heap allocations per chunk** over the blocking
 * version, measured -- one coroutine frame each for read_line, read_exactly
 * and eat_crlf, awaited per round.  HALO does not elide them: the handle is
 * laundered through task<T>, so the compiler cannot prove the frame does not
 * escape.
 *
 * For scale, the blocking version already costs about eleven per chunk, most
 * of it try_parse on the chunk size copying its input into an arena.  If
 * chunked bodies ever matter, that eleven is the number to go after and it has
 * nothing to do with coroutines.
 */
sys::task<void> read_body(sys::async_reader& in, framing how,
                          std::size_t length, async_body_sink sink,
                          std::size_t cap)
{
    switch(how) {
    case framing::none:
        co_return;

    case framing::length:
        refuse_oversized_claim(length, cap);

        co_await pour(in, length, sink);

        co_return;

    case framing::chunked: {
        std::size_t so_far = 0;

        for(;;) {
            const std::size_t n = chunk_size_of(co_await read_line(in, 4096));

            if(n == 0) {
                // The trailer section, read and discarded.  Something has to
                // consume it or the connection is not at a message boundary,
                // and a caller reusing it reads a trailer as a status line.
                for(;;) {
                    const std::string trailer = co_await read_line(in, 4096);

                    if(trailer.empty()) break;
                }

                co_return;
            }

            refuse_oversized_chunk(so_far, n, cap);

            so_far += n;

            if(!co_await pour(in, n, sink)) co_return;

            co_await eat_crlf(in);
        }
    }

    case framing::until_close: {
        char buf[BODY_BLOCK];

        std::size_t so_far = 0;

        for(;;) {
            const std::size_t took = in.take(buf, sizeof buf);

            if(took == 0) {
                if(!co_await in.fill()) co_return;

                continue;
            }

            refuse_oversized_stream(so_far + took, cap);

            so_far += took;

            if(!co_await sink(std::string_view(buf, took))) co_return;
        }
    }
    }

    co_return;
}

sys::task<std::string> read_body(sys::async_reader& in, framing how,
                                 std::size_t length, std::size_t cap)
{
    std::string body;

    co_await read_body(in, how, length,
                       [&body](std::string_view piece) -> sys::task<bool> {
                           body.append(piece);

                           co_return true;
                       },
                       cap);

    co_return body;
}

/** read_body() against what a response head said.  See the definition above. */
sys::task<std::string> read_body(sys::async_reader& in, const Response& head,
                                 std::size_t cap)
{
    co_return co_await read_body(in, head.body_framing(), head.content_length(),
                                 cap);
}

}
}
}
