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

// An HTTP/1.1 response head, read against RFC 9112's grammar.
//
// No server here: read_head() takes an std::istream, so an istringstream is
// the whole harness, and the awkward cases -- a head with no end, both framing
// fields at once, a chunk whose data looks like a chunk header -- can be
// written down exactly rather than coaxed out of something real.
//
// sys_proxy_live_test is the other half: it drives the same reader against a
// real tinyproxy, because a server nobody wrote produces responses nobody
// thought of.

#include <jlib/util/http.hh>
#include <jlib/util/URL.hh>
#include <jlib/util/abnf.hh>

#include <iostream>
#include <sstream>
#include <string>

namespace http = jlib::util::http;

using jlib::util::URL;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Did parsing this head throw? */
static bool refused(const std::string& head) {
    try {
        http::parse_head(head);

        return false;
    }
    catch(http::error&) {
        return true;
    }
}

static void the_grammar_is_the_whole_rfc() {
    std::cout << "\nthe grammar is the whole of both RFCs:\n";

    const jlib::util::abnf::grammar& g = http::grammar();

    // 3986 + 9110 + 9112, pasted whole rather than the dozen rules the client
    // uses.  If this number moves, a rule was added or lost and the reader
    // checking this file against the RFCs deserves to know.
    ok("it compiles, and has every rule in it", g.rules().size() == 194,
       std::to_string(g.rules().size()));

    ok("nothing is referenced and undefined", g.undefined().empty());

    // check() finds undefined references, left recursion and nullable
    // repetition.  It does NOT find an alternation that commits to the wrong
    // branch, which is the failure every grammar in this tree has actually
    // had -- so this passing is necessary and a long way from sufficient.
    bool checked = true;

    try { g.check(); } catch(std::exception&) { checked = false; }

    ok("and it passes check()", checked);

    // Exactly three rules are still prose, and they are the three RFC 9110
    // imports from documents jlib does not have: RFC 4647, RFC 5646, RFC 5322.
    // Anything else appearing here means an import was missed and a rule that
    // looks defined will fail at parse time.
    const std::vector<std::string> prose = g.prose_rules();

    ok("three rules are still prose, and only three", prose.size() == 3,
       std::to_string(prose.size()));

    bool expected = prose.size() == 3;

    for(const std::string& p : prose) {
        if(p != "language-range" && p != "language-tag" && p != "mailbox")
            expected = false;
    }

    ok("and they are the ones from RFCs jlib has not pasted", expected);
}

static void a_status_line_and_a_field_section() {
    std::cout << "\na status line and a field section:\n";

    const http::Response r = http::parse_head(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Date: Mon, 01 Jan 2024 00:00:00 GMT\r\n"
        "Content-Length: 12\r\n"
        "\r\n");

    // A number, not a string.  http::get() compared the status against the
    // string "200" and treated everything else as an error, which is why it
    // could neither follow a redirect nor read the body of a 400.
    ok("the status is a number", r.status() == 200, std::to_string(r.status()));
    ok("the version is there", r.version() == "HTTP/1.1", r.version());
    ok("and the reason phrase", r.reason() == "OK", r.reason());
    ok("2xx is ok()", r.ok());

    // The field value comes out of the match, and field-line puts the OWS
    // outside field-value, so it arrives trimmed without anyone trimming it.
    ok("a field value has no leading space",
       r.fields().get("Content-Type") == "text/html; charset=utf-8",
       "\"" + r.fields().get("Content-Type") + "\"");

    // RFC 9110 5.1: field names are case-insensitive.
    ok("names are matched without case",
       r.fields().has("content-type") && r.fields().has("CONTENT-TYPE"));

    // The one that proves the field-content fix.  As RFC 9110 publishes it,
    // field-content cannot match past a space under possessive repetition, and
    // this value would have read as "Mon,".
    ok("a value with spaces in it survives",
       r.fields().get("Date") == "Mon, 01 Jan 2024 00:00:00 GMT",
       "\"" + r.fields().get("Date") + "\"");

    ok("Content-Length gives length framing",
       r.body_framing() == http::framing::length && r.content_length() == 12,
       std::to_string(r.content_length()));

    // RFC 9112 4: the second SP is required even with no reason phrase.
    // Servers omit it, so this is a deliberate and narrow leniency.
    const http::Response bare = http::parse_head("HTTP/1.1 204\r\n\r\n");

    ok("a status line with no trailing space is read anyway",
       bare.status() == 204 && bare.reason().empty());

    // 204 has no body however the fields are written (9112 6.3).
    ok("and a 204 has no body", bare.body_framing() == http::framing::none);

    const http::Response err = http::parse_head(
        "HTTP/1.1 400 Bad Request\r\nContent-Length: 27\r\n\r\n");

    // A non-2xx is not an exception.  A token endpoint answers 400 with a body
    // saying whether the refresh token was revoked or the server hiccuped.
    ok("a 400 parses, with its body still to come",
       err.status() == 400 && !err.ok() && err.content_length() == 27);
}

static void what_rfc_9112_section_6_refuses() {
    std::cout << "\nwhat RFC 9112 section 6 refuses:\n";

    // The request-smuggling primitive.  Two recipients disagreeing about where
    // a message ends is how one request becomes two.
    ok("both Content-Length and Transfer-Encoding",
       refused("HTTP/1.1 200 OK\r\n"
               "Content-Length: 5\r\n"
               "Transfer-Encoding: chunked\r\n\r\n"));

    ok("two Content-Lengths that disagree",
       refused("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n"));

    // Two that agree are redundant rather than dangerous, and 9112 6.3 says to
    // take the value.
    bool agreed = false;

    try {
        const http::Response r = http::parse_head(
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n");

        agreed = r.content_length() == 5;
    }
    catch(http::error&) {}

    ok("but two that agree are allowed", agreed);

    ok("a Content-Length that is not a number",
       refused("HTTP/1.1 200 OK\r\nContent-Length: 5, 6\r\n\r\n"));

    // The grammar refuses these on its own -- field-line is
    // field-name ":" OWS field-value OWS and field-name is a token.
    ok("whitespace before the colon",
       refused("HTTP/1.1 200 OK\r\nContent-Length : 5\r\n\r\n"));

    ok("a field name that is not a token",
       refused("HTTP/1.1 200 OK\r\nCon tent: 5\r\n\r\n"));

    // 9112 5.2: obs-fold must be rejected or replaced by a recipient.
    ok("obs-fold, the obsolete continuation line",
       refused("HTTP/1.1 200 OK\r\nX-Long: one\r\n  two\r\n\r\n"));

    // 9112 2.2 permits accepting a bare LF as a terminator and forbids a bare
    // CR.  jlib accepts neither: the only way not to disagree with the
    // recipient in front of you is to insist on CRLF.
    ok("a bare LF as a line ending", refused("HTTP/1.1 200 OK\nX: 1\n\n"));

    ok("a bare CR inside a field value",
       refused("HTTP/1.1 200 OK\r\nX: one\rtwo\r\n\r\n"));

    ok("something that is not a status line at all",
       refused("220 mail.example.com ESMTP\r\n\r\n"));

    ok("and a status code that is not three digits",
       refused("HTTP/1.1 20 OK\r\n\r\n"));
}

static void reading_a_head_off_a_stream() {
    std::cout << "\nreading a head off a stream:\n";

    std::istringstream is("HTTP/1.1 200 OK\r\nX: 1\r\n\r\nbody goes here");

    const std::string head = http::read_head(is);

    ok("it stops at the blank line", head == "HTTP/1.1 200 OK\r\nX: 1\r\n\r\n",
       std::to_string(head.size()) + " octets");

    std::string rest;
    std::getline(is, rest);

    ok("and leaves the body in the stream", rest == "body goes here", rest);

    // A head with no end to it is an unbounded read from a stranger.  This is
    // the cap open_proxy() had and the reason its reader was worth keeping.
    std::string endless = "HTTP/1.1 200 OK\r\n";

    for(int i = 0; i < 500; i++) endless += "X-Padding: aaaaaaaaaaaaaaaaaaaa\r\n";

    std::istringstream big(endless);
    bool capped = false;

    try { http::read_head(big, 4096); }
    catch(http::error&) { capped = true; }

    ok("a head with no end is given up on", capped);

    std::istringstream cut("HTTP/1.1 200 OK\r\nX: 1\r\n");
    bool truncated = false;

    try { http::read_head(cut); }
    catch(http::error&) { truncated = true; }

    ok("and so is one the connection cut short", truncated);
}

static void reading_a_body() {
    std::cout << "\nreading a body:\n";

    {
        std::istringstream is("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloAND MORE");

        const http::Response head = http::read_response_head(is);

        ok("Content-Length octets, and no more",
           http::read_body(is, head) == "hello");
    }

    {
        // The case that catches a chunked reader written with find(): the
        // chunk data contains a line that looks exactly like a chunk header.
        // Framing by the counts is what makes this work, and framing by
        // searching is what does not.
        const std::string trap = "5\r\nnope\r\n";

        std::ostringstream o;

        o << "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
          << std::hex << trap.size() << "\r\n" << trap << "\r\n"
          << "3\r\nend\r\n"
          << "0\r\n\r\n";

        std::istringstream is(o.str());

        const http::Response head = http::read_response_head(is);

        ok("Transfer-Encoding: chunked gives chunked framing",
           head.body_framing() == http::framing::chunked);

        const std::string body = http::read_body(is, head);

        ok("a chunk whose data looks like a chunk header reassembles",
           body == trap + "end", body);
    }

    {
        // A chunk extension is parsed off and discarded, and so is the trailer
        // section -- something has to consume the trailer or the connection is
        // not at a message boundary and the next reader takes it for a status
        // line.
        std::istringstream is(
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
            "4;name=value\r\nabcd\r\n"
            "0\r\nX-Checksum: 1234\r\n\r\n"
            "HTTP/1.1 200 OK\r\n\r\n");

        const http::Response head = http::read_response_head(is);

        ok("a chunk extension is ignored", http::read_body(is, head) == "abcd");

        // And the proof that the trailer was consumed: what is left is the
        // next message, starting exactly where it should.
        const http::Response next = http::read_response_head(is);

        ok("and the trailer section is consumed", next.status() == 200);
    }

    {
        std::istringstream is("HTTP/1.1 200 OK\r\nX: 1\r\n\r\nall of this");

        const http::Response head = http::read_response_head(is);

        ok("with no framing fields the body runs to end of stream",
           head.body_framing() == http::framing::until_close &&
           http::read_body(is, head) == "all of this");
    }

    {
        // A Content-Length from a stranger decides an allocation.
        std::istringstream is("HTTP/1.1 200 OK\r\nContent-Length: 999999999\r\n\r\nx");

        const http::Response head = http::read_response_head(is);
        bool capped = false;

        try { http::read_body(is, head, 1024); }
        catch(http::error&) { capped = true; }

        ok("an enormous Content-Length is refused rather than allocated", capped);
    }
}

static void a_relative_reference_parses_now() {
    std::cout << "\na relative reference parses now:\n";

    // RFC 9110 makes Location a URI-reference, and a 302 pointing at "/signin"
    // is the ordinary case.  relative-ref was simply not in the pasted grammar
    // -- the first version of rfc3986.hh took only the rules URL::parse needed.
    ok("a path-only reference", URL::valid_reference("/signin?next=/inbox"));
    ok("a reference with no leading slash", URL::valid_reference("next/page"));
    ok("and one beginning with //", URL::valid_reference("//example.com/x"));

    URL u;
    u.parse_reference("/signin?next=%2Finbox");

    ok("it has no scheme, and says so", u.relative() && u.get_protocol().empty());
    ok("and the path is there", u.get_path() == "/signin", u.get_path());

    // parse() is deliberately NOT widened.  "imap:/host" is a typo and also a
    // perfectly good relative reference; a parse() that accepted it would stop
    // throwing and start handing Imap4 something with no host to connect to.
    ok("but parse() still refuses a relative reference",
       !URL::valid("example.com/x"));

    ok("and an absolute URL goes through either", URL::valid_reference("https://x/y") &&
       URL::valid("https://x/y"));
}

int main() {
    std::cout << std::unitbuf;

    the_grammar_is_the_whole_rfc();
    a_status_line_and_a_field_section();
    what_rfc_9112_section_6_refuses();
    reading_a_head_off_a_stream();
    reading_a_body();
    a_relative_reference_parses_now();

    // What a green run does not establish.
    //
    // That the rules nothing here exercises are right.  RFC 9110 and RFC 9112
    // are pasted whole and this uses a fraction of them; check() proves they
    // are well-formed, not that each alternation commits to the branch the RFC
    // meant.  Every grammar in this tree that needed reordering -- dec-octet,
    // IPv6address, RFC 5234's own defined-as and repeat, five of RFC 5322's,
    // and field-content on this branch -- was found by running it, never by
    // reading it.  Two are known wrong and written down in rfc9110.hh:
    // credentials/challenge, and chunk.
    //
    // That a real server sends what this expects.  Everything here is an
    // istringstream, which produces exactly the responses somebody thought of.
    // sys_proxy_live_test runs the same reader against tinyproxy, which is one
    // real server and not a survey.
    //
    // Anything about requests.  read_head parses a response; request-line and
    // the four request-target forms compile and are unexercised.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
