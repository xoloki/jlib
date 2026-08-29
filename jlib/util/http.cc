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

std::string read_head(std::istream& is, std::size_t cap) {
    std::string head;

    // One octet at a time, looking for the blank line.  Not getline: a bare LF
    // in the middle of the section is something this has to be able to see and
    // refuse, and getline would silently make it a line ending.
    //
    // "\n\n" is accepted as an end as well as "\r\n\r\n" only so that a head
    // sent with bare LFs is *read* and then *rejected* by the grammar, with a
    // message saying what was wrong, rather than read until the cap runs out.
    while(head.size() < 4 ||
          (head.compare(head.size() - 4, 4, "\r\n\r\n") != 0 &&
           head.compare(head.size() - 2, 2, "\n\n") != 0)) {
        const int c = is.get();

        if(c == std::char_traits<char>::eof()) {
            throw error("the connection closed before the message head ended, "
                        "after " + std::to_string(head.size()) + " octets");
        }

        head += static_cast<char>(c);

        if(head.size() > cap) {
            throw error("no end to the message head within " +
                        std::to_string(cap) + " octets");
        }
    }

    return head;
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
     */
    std::vector<std::string> split_lines(std::string_view head) {
        std::vector<std::string> lines;
        std::size_t i = 0;

        while(i < head.size()) {
            const std::size_t nl = head.find("\r\n", i);

            if(nl == std::string_view::npos)
                throw error("the response head does not end in CRLF");

            const std::string_view line = head.substr(i, nl - i);

            if(line.find('\r') != std::string_view::npos ||
               line.find('\n') != std::string_view::npos) {
                throw error("a bare CR or LF inside the response head");
            }

            if(line.empty()) {
                // The blank line.  Anything after it is the body's business.
                return lines;
            }

            lines.emplace_back(line);
            i = nl + 2;
        }

        throw error("the response head has no blank line at the end of it");
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
    void read_fields(const std::vector<std::string>& lines, std::size_t from,
                     fields& into)
    {
        const abnf::rule field = grammar().at("field-line");

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

Request parse_request_head(std::string_view head) {
    const std::vector<std::string> lines = split_lines(head);

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

Response parse_head(std::string_view head, bool head_request) {
    const std::vector<std::string> lines = split_lines(head);

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

// --------------------------------------------------------------- read_body

namespace {

    std::string read_exactly(std::istream& is, std::size_t n, std::size_t cap) {
        if(n > cap) {
            throw error("the body claims " + std::to_string(n) +
                        " octets and the most that will be read is " +
                        std::to_string(cap));
        }

        std::string out(n, '\0');

        is.read(&out[0], static_cast<std::streamsize>(n));

        if(static_cast<std::size_t>(is.gcount()) != n) {
            throw error("the connection closed " +
                        std::to_string(n - is.gcount()) +
                        " octets short of the body it promised");
        }

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

}

std::string read_body(std::istream& is, const Response& head, std::size_t cap) {
    return read_body(is, head.body_framing(), head.content_length(), cap);
}

std::string read_body(std::istream& is, framing how, std::size_t length,
                      std::size_t cap)
{
    switch(how) {
    case framing::none:
        return std::string();

    case framing::length:
        return read_exactly(is, length, cap);

    case framing::chunked: {
        std::string body;

        for(;;) {
            // chunk-size [ chunk-ext ] CRLF -- the extension is parsed off and
            // thrown away, which is what RFC 9112 7.1.1 says to do with one
            // that is not recognised, and none is.
            const std::string line = read_line(is, 4096);
            const std::size_t semi = line.find(';');
            const std::string size_text = line.substr(0, semi);

            if(!grammar().at("chunk-size").try_parse(size_text))
                throw error("not a chunk size: \"" + size_text + "\"");

            const std::size_t n = std::stoull(size_text, 0, 16);

            if(n == 0) {
                // The trailer section, read and discarded.  Something has to
                // consume it or the connection is not at a message boundary,
                // and a caller reusing it reads a trailer as a status line.
                for(;;) {
                    const std::string trailer = read_line(is, 4096);

                    if(trailer.empty()) break;
                }

                return body;
            }

            if(body.size() + n > cap) {
                throw error("a chunked body is larger than the " +
                            std::to_string(cap) + " octets that will be read");
            }

            body += read_exactly(is, n, cap);

            eat_crlf(is);
        }
    }

    case framing::until_close: {
        std::string body;
        char buf[4096];

        while(is.read(buf, sizeof buf) || is.gcount() > 0) {
            if(body.size() + is.gcount() > cap) {
                throw error("a body read to end of stream passed the " +
                            std::to_string(cap) + " octets that will be read");
            }

            body.append(buf, is.gcount());
        }

        return body;
    }
    }

    return std::string();
}

}
}
}
