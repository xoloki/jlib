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

// IMAP4rev1 responses, read against RFC 3501 section 9.
//
// The section worth reading is "a response is not a line".  Everything else
// here is a grammar doing what a grammar does; that one is the bug this was
// written for, and it is the reason jlib::util::abnf has two public layers.

#include <jlib/net/imap_response.hh>
#include <jlib/net/rfc3501.hh>

#include <jlib/util/abnf.hh>

#include <iostream>
#include <sstream>
#include <string>

namespace imap = jlib::net::imap;

using imap::response;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static std::string show(const std::string& s) {
    std::string out;

    for(char c : s) {
        if(c == '\r')      out += "\\r";
        else if(c == '\n') out += "\\n";
        else               out += c;
    }

    return out;
}

static void a_response_is_not_a_line() {
    std::cout << "a response is not a line:\n";

    // RFC 3501 4.3: a literal is "{" number "}" CRLF and then exactly that
    // many octets.  They are message content, so they may contain anything --
    // including a line that looks exactly like the tagged completion the
    // client is waiting for.
    //
    // Imap4::handshake read lines until one began with the tag.  Given this
    // stream it stopped inside the message, and every command after it read
    // the rest of that message as its own response.
    std::istringstream is(
        "* 12 FETCH (BODY[HEADER] {33}\r\n"
        "a001 OK this looks like the end\r\n"   // 33 octets of content
        ")\r\n"
        "a001 OK FETCH completed\r\n");

    const std::string first = imap::read(is);

    ok("the whole FETCH is one response",
       first == "* 12 FETCH (BODY[HEADER] {33}\r\n"
                "a001 OK this looks like the end\r\n)\r\n",
       show(first));

    const std::string second = imap::read(is);

    ok("and the real completion is the next one",
       second == "a001 OK FETCH completed\r\n", show(second));

    // The parsed form gets the content back exactly.
    const response r = response::parse(first);

    ok("the literal's octets come back whole",
       r.attributes().at("BODY[HEADER]") == "a001 OK this looks like the end\r\n",
       show(r.attributes().count("BODY[HEADER]")
            ? r.attributes().at("BODY[HEADER]") : std::string()));

    // Two literals in one response, which LIST does when a mailbox name needs
    // one and so does a FETCH of more than one part.
    std::istringstream two(
        "* 12 FETCH (BODY[1] {5}\r\nhello BODY[2] {5}\r\nworld)\r\n"
        "a002 OK done\r\n");

    const response t = response::parse(imap::read(two));

    ok("and two of them in one response",
       t.attributes().count("BODY[1]") && t.attributes().at("BODY[1]") == "hello" &&
       t.attributes().count("BODY[2]") && t.attributes().at("BODY[2]") == "world");

    // A truncated stream is not a short response.
    std::istringstream cut("* 12 FETCH (BODY[] {100}\r\nnot a hundred octets\r\n");

    bool threw = false;

    try { imap::read(cut); }
    catch(imap::error&) { threw = true; }

    ok("a connection that drops inside a literal throws", threw);
}

static void the_literal_introducer() {
    std::cout << "\nwhere a literal starts:\n";

    struct { const char* line; bool found; std::size_t n; } cases[] = {
        { "* 12 FETCH (RFC822 {1234}",   true, 1234 },
        { "* 12 FETCH (RFC822 {0}",      true, 0 },
        { "* 12 FETCH (RFC822 {1234+}",  true, 1234 },   // RFC 7888
        { "* 12 FETCH (FLAGS (\\Seen))", false, 0 },
        { "a001 OK FETCH completed",     false, 0 },
        { "",                            false, 0 },
        { "* 12 FETCH {}",               false, 0 },
        { "* 12 FETCH {abc}",            false, 0 },
        { "* 12 FETCH {12} and more",    false, 0 },
        { "* 12 FETCH {2147483648}",     false, 0 },
        { "* 12 FETCH {2147483647}",     true, 2147483647u },
    };

    for(const auto& c : cases) {
        std::size_t n = 12345;

        ok(std::string("\"") + c.line + "\"",
           imap::literal_size(c.line, n) == c.found && (!c.found || n == c.n));
    }
}

static void the_three_shapes() {
    std::cout << "\nRFC 3501 7, the three shapes:\n";

    const response t = response::parse("a001 OK LOGIN completed\r\n");

    ok("a tagged completion",
       t.type() == response::kind::tagged && t.tag() == "a001" && t.ok(),
       t.tag());
    ok("with its text", t.text() == "LOGIN completed", t.text());

    const response n = response::parse("a002 NO [ALERT] mailbox is full\r\n");

    ok("a failure is not ok()", !n.ok() &&
       n.status() == response::condition::no);
    ok("and its response-text-code comes out of its brackets",
       n.code() == "ALERT" && n.text() == "mailbox is full", n.code());

    const response u = response::parse("* OK [UIDVALIDITY 3857529045] UIDs valid\r\n");

    ok("an untagged condition",
       u.type() == response::kind::untagged && u.ok() &&
       u.code() == "UIDVALIDITY 3857529045", u.code());

    const response c = response::parse("+ Ready for additional command text\r\n");

    ok("a continuation", c.type() == response::kind::continuation &&
       c.text() == "Ready for additional command text", c.text());

    ok("and an empty one, which AUTHENTICATE gets",
       response::valid("+ \r\n") && response::valid("+\r\n"));

    // PREAUTH and BYE are conditions the RFC keeps in separate productions
    // because of where they may appear; they read the same way.
    ok("PREAUTH is ok()",
       response::parse("* PREAUTH IMAP4rev1 ready\r\n").ok());
    ok("BYE is not",
       !response::parse("* BYE Autologout\r\n").ok() &&
       response::parse("* BYE Autologout\r\n").status() == response::condition::bye);
}

static void the_data_responses() {
    std::cout << "\nwhat an untagged response carries:\n";

    {
        const response r = response::parse("* CAPABILITY IMAP4rev1 STARTTLS AUTH=GSSAPI\r\n");

        ok("CAPABILITY", r.name() == "CAPABILITY" && r.capabilities().size() == 3 &&
           r.capabilities()[2] == "AUTH=GSSAPI",
           r.capabilities().empty() ? "none" : r.capabilities()[2]);
    }

    {
        const response r = response::parse("* FLAGS (\\Answered \\Seen $Label1)\r\n");

        ok("FLAGS, including one that is not a system flag",
           r.name() == "FLAGS" && r.flags().size() == 3 && r.flags()[2] == "$Label1",
           r.flags().empty() ? "none" : r.flags()[2]);
    }

    {
        const response r =
            response::parse("* LIST (\\Noselect \\HasChildren) \"/\" \"My Mail\"\r\n");

        ok("LIST", r.name() == "LIST" && r.mailbox() == "My Mail" &&
           r.delimiter() == "/" && r.flags().size() == 2, r.mailbox());
    }

    {
        // A NIL delimiter means the mailbox has no hierarchy.
        const response r = response::parse("* LSUB () NIL INBOX\r\n");

        ok("LSUB with a NIL delimiter",
           r.name() == "LSUB" && r.mailbox() == "INBOX" &&
           r.delimiter().empty() && r.flags().empty(), r.mailbox());
    }

    {
        // A mailbox name that needs a literal, which is what the line-based
        // reader could not do at all.
        const response r = response::parse("* LIST () \"/\" {8}\r\nMy Mail!\r\n");

        ok("a mailbox name in a literal", r.mailbox() == "My Mail!", r.mailbox());
    }

    ok("EXISTS",  response::parse("* 172 EXISTS\r\n").number() == 172);
    ok("RECENT",  response::parse("* 1 RECENT\r\n").number() == 1);
    ok("EXPUNGE", response::parse("* 44 EXPUNGE\r\n").name() == "EXPUNGE" &&
                  response::parse("* 44 EXPUNGE\r\n").number() == 44);

    {
        const response r = response::parse("* SEARCH 2 84 882\r\n");

        ok("SEARCH", r.name() == "SEARCH" && r.numbers().size() == 3 &&
           r.numbers()[1] == 84);
    }

    ok("and a SEARCH that found nothing",
       response::parse("* SEARCH\r\n").numbers().empty());
}

static void fetch_attributes() {
    std::cout << "\nFETCH:\n";

    const response r = response::parse(
        "* 12 FETCH (FLAGS (\\Seen) RFC822.SIZE 44827 UID 4827313)\r\n");

    ok("the message number", r.name() == "FETCH" && r.number() == 12);
    ok("the flags",          r.flags().size() == 1 && r.flags()[0] == "\\Seen");
    ok("a numeric attribute", r.attributes().at("RFC822.SIZE") == "44827");
    ok("and the uid",         r.attributes().at("UID") == "4827313");

    // The section is part of the key, because it is part of what was asked
    // for: a caller that fetched BODY[HEADER] looks for BODY[HEADER].
    const response b = response::parse(
        "* 12 FETCH (BODY[HEADER.FIELDS (FROM TO)] {13}\r\nFrom: a@b.c\r\n)\r\n");

    ok("a section is part of the name",
       b.attributes().count("BODY[HEADER.FIELDS (FROM TO)]") == 1,
       b.attributes().empty() ? "none" : b.attributes().begin()->first);

    // A quoted string, with the escaping RFC 3501 4.3 allows: only DQUOTE and
    // backslash, and each stands for itself.
    const response q = response::parse(
        "* 1 FETCH (RFC822.TEXT \"say \\\" hi\\\\\")\r\n");

    ok("a quoted value is unquoted and unescaped",
       q.attributes().at("RFC822.TEXT") == "say \" hi\\",
       q.attributes().at("RFC822.TEXT"));

    ok("NIL is empty", response::parse("* 1 FETCH (RFC822.TEXT NIL)\r\n")
                           .attributes().at("RFC822.TEXT").empty());

    // ENVELOPE and BODYSTRUCTURE are sixty productions jlib does not
    // interpret, so they come back as written rather than half-understood.
    // An early version of the reader reached inside for the first string it
    // found and turned this into "d".
    const response e = response::parse(
        "* 12 FETCH (ENVELOPE (\"d\" \"s\" NIL NIL))\r\n");

    ok("a parenthesised value is handed over whole",
       e.attributes().at("ENVELOPE") == "(\"d\" \"s\" NIL NIL)",
       e.attributes().at("ENVELOPE"));

    // An attribute nobody here has heard of does not fail the response.  This
    // matters: servers volunteer MODSEQ, X-GM-MSGID and others unasked.
    const response x = response::parse(
        "* 12 FETCH (MODSEQ (12345) X-GM-MSGID 1278455344230334865 UID 9)\r\n");

    ok("an unknown attribute parses rather than failing",
       x.attributes().at("MODSEQ") == "(12345)" &&
       x.attributes().at("X-GM-MSGID") == "1278455344230334865" &&
       x.attributes().at("UID") == "9");

    ok("and a partial fetch's origin octet",
       response::parse("* 1 FETCH (BODY[]<0> \"x\")\r\n")
           .attributes().count("BODY[]<0>") == 1);
}

static void what_it_refuses() {
    std::cout << "\nnot a response:\n";

    for(const char* s : { "garbage\r\n", "\r\n", "",
                          "* 12 FETCH (FLAGS (\\Seen)\r\n",     // unbalanced
                          "* 12 FETCH\r\n",                     // no attributes
                          "a001 MAYBE something\r\n",           // no such condition
                          "* LIST (\\Noselect) \"/\"\r\n",      // no mailbox
                          "a001 OK no newline" }) {
        ok(std::string("\"") + show(s) + "\"", !response::valid(s));
    }

    std::string msg;

    try { response::parse("garbage\r\n"); }
    catch(imap::error& e) { msg = e.what(); }

    ok("and the message says where", msg.find("column") != std::string::npos, msg);
}

static void the_grammar_itself() {
    std::cout << "\nthe grammar:\n";

    using namespace jlib::util::abnf;

    // It does not compile on its own: literal is referenced and never
    // defined, because RFC 3501 4.3 puts the length constraint in a comment.
    grammar g = compile(jlib::net::rfc3501::RESPONSE);

    bool undefined = false;

    try { g.check(); }
    catch(grammar_error&) { undefined = true; }

    ok("literal is left for the combinators to define", undefined);

    // And this is what imap_response.cc does, which is the whole argument for
    // both layers of abnf being public.
    const rule size = as("literal-size", +core::DIGIT());

    g.define("literal", lit("{") >> size >> lit("}") >> core::CRLF()
                       >> as("literal-text", counted(size)));

    bool built = false;
    std::string why;

    try { g.check(); built = true; }
    catch(exception& e) { why = e.what(); }

    ok("and then it checks", built, why);

    // ATOM-CHAR is stated in the RFC as an exclusion in prose, so a reader
    // cannot check it by comparing.  Every excluded printable character.
    // Tested in an unquoted mailbox name -- which is an astring, so the run
    // stops at the first character it may not contain and the CRLF that has
    // to follow is not there.
    //
    // Not in a flag list, which is where this was first written: a space
    // there is the separator between two flags, so "(a b)" is two flags and
    // proves nothing at all.
    for(char c : std::string(" (){%*\"\\")) {
        const std::string s = std::string("* LSUB () NIL a") + c + "b\r\n";

        ok(std::string("ATOM-CHAR excludes '") + c + "'", !response::valid(s));
    }

    ok("a space in a flag list is a separator, not a flag character",
       response::valid("* FLAGS (a b)\r\n") &&
       response::parse("* FLAGS (a b)\r\n").flags().size() == 2);

    // "]" is resp-specials: not an ATOM-CHAR, but an ASTRING-CHAR.
    ok("and \"]\", which an astring may still contain",
       !response::valid("* FLAGS (a]b)\r\n") &&
       response::valid("* LSUB () NIL a]b\r\n"));
}

int main() {
    std::cout << std::unitbuf;

    a_response_is_not_a_line();
    the_literal_introducer();
    the_three_shapes();
    the_data_responses();
    fetch_attributes();
    what_it_refuses();
    the_grammar_itself();

    // What a green run does NOT establish.
    //
    // Not that jlib speaks IMAP.  No server was involved: these are strings
    // and a std::istringstream.  What is established is that a response with a
    // literal in it is read whole, which is the thing the line-based reader
    // could not do.
    //
    // Not RFC 3501 conformance.  The commands are not here -- jlib writes
    // those and knows what it wrote -- and ENVELOPE, BODYSTRUCTURE and the
    // STATUS attributes are matched by shape rather than by their sixty-odd
    // productions, because jlib does not interpret them.  rfc3501.hh says
    // exactly which departures there are.
    //
    // Not extensions.  This is RFC 3501, not 4466, 4551, 6154 or 9051.  An
    // extension's attribute parses as a name and a value, which is the most a
    // client that has not implemented it should do.
    //
    // Not NIL.  RFC 3501 distinguishes NIL from the empty string and a
    // std::string cannot; both come back as "".
    //
    // Not the rest of Imap4.  handshake() reads responses now rather than
    // lines, so every command benefits -- but its callers still take the
    // response apart with util::tokenize and a linear scan.  Putting them on
    // response's accessors, and implementing SEARCH, UID and a ranged FETCH
    // against it, is the other half of #85.
    return failures ? 1 : 0;
}
