/* -*- mode: C++ c-basic-offset: 4  -*-
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
 *
 */

// Framing: where a message ends, and how a byte that looks like the end is
// kept from being mistaken for one.
//
// SMTP and POP3 both terminate a body with a "." on a line by itself and both
// call the escaping that makes that safe "transparency"; IMAP does not, and
// counts octets instead.  All three were wrong here in the same way -- each
// trusted something that is not the framing to tell it where the framing was.

#include <jlib/net/net.hh>
#include <jlib/net/Pop3.hh>
#include <jlib/net/imap_response.hh>

#include <iostream>
#include <sstream>
#include <string>

using jlib::net::dot_stuff;
using jlib::net::dot_unstuff;
using jlib::net::Pop3;
namespace imap = jlib::net::imap;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** CRLF and the escapes made visible, so a failure says what it got. */
static std::string show(const std::string& s) {
    std::string out;

    for(char c : s) {
        if(c == '\r')      out += "\\r";
        else if(c == '\n') out += "\\n";
        else               out += c;
    }

    return out;
}

static void smtp_transparency() {
    std::cout << "dot-stuffing, RFC 5321 4.5.2:\n";

    // The case the whole mechanism exists for, and the one the old code did
    // nothing about.  ".signature" is a line that starts with a dot and is not
    // a lone dot; unstuffed, the server sees ".signature" mid-body, which is
    // not the terminator, so this one is only *half* the story --
    ok("a line beginning with a dot gets another",
       dot_stuff("hello\r\n.signature\r\n") == "hello\r\n..signature\r\n",
       show(dot_stuff("hello\r\n.signature\r\n")));

    // -- and this is the other half.  A lone "." inside the body ends the
    // DATA command where it stands, and everything after it is read by the
    // server as SMTP commands.
    ok("and so does a line that is only a dot",
       dot_stuff("hello\r\n.\r\nworld\r\n") == "hello\r\n..\r\nworld\r\n",
       show(dot_stuff("hello\r\n.\r\nworld\r\n")));

    // The first line is a line.
    ok("including the first line",
       dot_stuff(".hello\r\nworld\r\n") == "..hello\r\nworld\r\n",
       show(dot_stuff(".hello\r\nworld\r\n")));

    // And a line with no dot is left exactly alone -- this is what the old
    // code got wrong in the other direction, rewriting "\r\n.\r\n" as
    // "\r\n. \r\n" and putting a space into the sender's message.
    ok("a body with no leading dots is untouched",
       dot_stuff("hello\r\nworld. and more\r\n") == "hello\r\nworld. and more\r\n");

    ok("an empty body is untouched", dot_stuff("").empty());

    ok("and a final line with no CRLF still counts",
       dot_stuff("a\r\n.b") == "a\r\n..b", show(dot_stuff("a\r\n.b")));

    // The two are inverses, which is the property that matters: what the
    // receiver reconstructs is what the sender wrote.
    for(const char* s : { "hello\r\n.signature\r\n", ".\r\n", "..\r\n...\r\n",
                          "a\r\n\r\n.\r\nb\r\n", "", ".", "no dots here\r\n" }) {
        ok(std::string("round trip: \"") + show(s) + "\"",
           dot_unstuff(dot_stuff(s)) == s, show(dot_unstuff(dot_stuff(s))));
    }
}

static void pop3_framing() {
    std::cout << "\nPOP3 multi-line responses, RFC 1939 3:\n";

    {
        std::istringstream is("From: a@b\r\n\r\nhello\r\n.\r\n+OK next command\r\n");

        const std::string body = Pop3::read_body(is);

        ok("the body ends at the dot",
           body == "From: a@b\r\n\r\nhello\r\n", show(body));
    }

    {
        // And the stream is left where the next command's response starts.
        // This is the failure the old code caused: it read the octet count out
        // of the "+OK 1234 octets" line, and where that count was absent,
        // approximate, or counting CRLF as one octet rather than two, the rest
        // of the message stayed in the socket and the next command read it as
        // its own response.
        std::istringstream is("hello\r\n.\r\n+OK 3 messages\r\n");
        Pop3::read_body(is);

        std::string next;
        std::getline(is, next);

        ok("and the stream is left at the next response",
           next == "+OK 3 messages\r", show(next));
    }

    {
        std::istringstream is("a\r\n..\r\n...b\r\n.\r\n");

        const std::string body = Pop3::read_body(is);

        ok("dots are unstuffed on the way past",
           body == "a\r\n.\r\n..b\r\n", show(body));
    }

    {
        // sys::getline erases *every* trailing CR, which is right for a
        // command response and wrong for message content.  read_body takes
        // exactly one CRLF off, so a body line that genuinely ends in CR
        // survives.
        std::istringstream is("ends in a cr\r\r\n.\r\n");

        const std::string body = Pop3::read_body(is);

        ok("a body line ending in CR keeps it",
           body == "ends in a cr\r\r\n", show(body));
    }

    {
        std::istringstream is(".\r\n");

        ok("an empty body is empty", Pop3::read_body(is).empty());
    }

    {
        // A truncated message is not a short message.  Returning what arrived
        // would hand a caller half an email with no indication of it.
        std::istringstream is("hello\r\nworld\r\n");

        bool threw = false;
        try { Pop3::read_body(is); }
        catch(Pop3::exception&) { threw = true; }

        ok("a connection that drops mid-body throws", threw);
    }
}

static void imap_literals() {
    std::cout << "\nIMAP literals, RFC 3501 4.3:\n";

    struct { const char* line; bool found; std::size_t n; } cases[] = {
        { "* 1 FETCH (FLAGS (\\Seen) RFC822.SIZE 4242 RFC822 {1234}", true, 1234 },
        { "* 1 FETCH (RFC822.HEADER {0}",                             true, 0 },

        // RFC 7888's non-synchronising literal.
        { "* 1 FETCH (RFC822 {1234+}",                                true, 1234 },

        // No literal at all.  This is the one that mattered: util::slice
        // returns its whole input when the delimiters are absent, so
        // int_value of it was 0, the code read zero octets, and then read the
        // message body as protocol lines.
        { "* 1 FETCH (FLAGS (\\Seen))",                               false, 0 },
        { "a001 OK FETCH completed",                                  false, 0 },
        { "",                                                         false, 0 },
        { "* 1 FETCH (RFC822 NIL)",                                   false, 0 },

        // Nearly a literal.
        { "* 1 FETCH {}",                                             false, 0 },
        { "* 1 FETCH {abc}",                                          false, 0 },
        { "* 1 FETCH {12x}",                                          false, 0 },
        { "* 1 FETCH {-1}",                                           false, 0 },
        { "* 1 FETCH {12",                                            false, 0 },

        // The introducer has to be the last thing on the line.
        { "* 1 FETCH {12} and then some",                             false, 0 },

        // A count that decides how many octets to read, from the network.
        { "* 1 FETCH {99999999999999999999}",                         false, 0 },
        { "* 1 FETCH {2147483648}",                                   false, 0 },
        { "* 1 FETCH {2147483647}",                                   true, 2147483647u },
    };

    for(const auto& c : cases) {
        std::size_t n = 12345;
        const bool got = imap::literal_size(c.line, n);

        ok(std::string("\"") + c.line + "\"",
           got == c.found && (!c.found || n == c.n),
           got ? std::to_string(n) : "no literal");
    }
}

int main() {
    std::cout << std::unitbuf;

    smtp_transparency();
    pop3_framing();
    imap_literals();

    // What a green run does NOT establish.
    //
    // Not that jlib speaks SMTP, POP3 or IMAP correctly.  Every one of these
    // is a pure function over a buffer, tested against a std::istringstream;
    // no server was involved and none of the connect, authenticate or command
    // sequencing around them is exercised here.  What is established is that
    // the framing decisions -- where a body ends, and which bytes are content
    // rather than protocol -- are now made by the rule the RFC states.
    //
    // Not the rest of Imap4.  literal_size() reads the introducer; the code
    // that finds RFC822.SIZE in a response is still util::tokenize and a
    // linear search, and a FETCH response is not really a whitespace-separated
    // list of tokens.  That wants RFC 3501's grammar the way addresses got RFC
    // 5322's, and it is not this branch.
    //
    // Not MIME.  Email.cc still finds a multipart boundary by splitting the
    // Content-Type on ";" -- which splits inside a quoted parameter value --
    // and then asking util::slice for what is between the quotes, which
    // returns the whole input when the value is unquoted.  Same bug class,
    // different RFC, next branch.
    //
    // Not the "+OK n octets" count.  read_body ignores it entirely rather than
    // checking the body against it.  That is deliberate: RFC 1939 makes the
    // number optional and servers disagree about whether it counts CRLF as one
    // octet or two, so it is not something to validate against.
    return failures ? 1 : 0;
}
