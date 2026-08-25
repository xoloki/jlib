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

// Content-Type, Content-Disposition, and where a multipart body divides.
//
// The last of the four headers jlib was reading by looking for a delimiter and
// hoping.  This one is the worst of them, because getting it wrong is not a
// parse error a caller can see -- the message simply does not come apart into
// its pieces, and an attachment goes missing.

#include <jlib/util/content_type.hh>
#include <jlib/util/rfc2045.hh>
#include <jlib/net/rfc5322.hh>
#include <jlib/net/Email.hh>

#include <jlib/util/abnf.hh>

#include <iostream>
#include <string>

using jlib::util::content_type;
using jlib::util::split_multipart;

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

/** get(name) or "<threw>", so a failure prints something. */
static std::string param(const char* header, const char* name) {
    try { return content_type::parse(header).get(name); }
    catch(content_type::exception&) { return "<threw>"; }
}

static void reading_a_content_type() {
    std::cout << "Content-Type:\n";

    const content_type c = content_type::parse("text/plain; charset=us-ascii");

    ok("type and subtype", c.type() == "text" && c.subtype() == "plain");
    ok("essence",          c.essence() == "text/plain", c.essence());
    ok("a parameter",      c.get("charset") == "us-ascii", c.get("charset"));
    ok("and one that is not there", c.get("boundary", "none") == "none");
    ok("has()",            c.has("charset") && !c.has("boundary"));

    // RFC 2045 5.1: the type, subtype and parameter names are case
    // insensitive.  The value of a parameter is not -- a boundary is case
    // sensitive, and so is a filename.
    const content_type u = content_type::parse("TEXT/PLAIN; CharSet=\"UTF-8\"");

    ok("names fold case",  u.essence() == "text/plain" && u.has("CHARSET"));
    ok("values do not",    u.get("charset") == "UTF-8", u.get("charset"));

    // RFC 822 comments, which RFC 2045 5.1 says are allowed here.
    ok("a comment is not part of anything",
       content_type::parse("text/plain (why not); charset=utf-8").get("charset")
       == "utf-8");

    ok("whitespace around the equals",
       param("text/plain; charset = utf-8", "charset") == "utf-8");

    // Not legal, and everywhere.
    ok("a trailing semicolon", content_type::valid("text/plain;"));
    ok("a doubled one",        content_type::valid("text/plain; ; charset=x"));

    for(const char* bad : { "text", "text/", "/plain", "text/plain; charset",
                            "text plain", "" }) {
        ok(std::string("\"") + bad + "\" is not a media type",
           !content_type::valid(bad));
    }
}

static void the_boundary_that_used_to_break_it() {
    std::cout << "\nthe one that mattered:\n";

    // Splitting the header on ";" cuts this in half, and the old code then
    // asked util::slice for what lay between the first quote and the last.
    // The boundary came out as "a" and the message did not divide.
    ok("a semicolon inside a quoted boundary",
       param("multipart/mixed; boundary=\"a;b\"", "boundary") == "a;b",
       param("multipart/mixed; boundary=\"a;b\"", "boundary"));

    // And the other half of that: slice returns its whole input when the
    // delimiters are absent, so an unquoted boundary came back with its own
    // name still attached.
    ok("an unquoted boundary",
       param("multipart/mixed; boundary=simple", "boundary") == "simple",
       param("multipart/mixed; boundary=simple", "boundary"));

    ok("one with an equals sign in it",
       param("multipart/mixed; boundary=\"----=_Part_0_1234\"", "boundary")
       == "----=_Part_0_1234");

    ok("and an escaped quote in a value",
       param("application/x; name=\"say \\\" hi\"", "name") == "say \" hi",
       param("application/x; name=\"say \\\" hi\"", "name"));
}

static void repeated_parameters() {
    std::cout << "\na parameter given twice:\n";

    // RFC 2045 does not say what this means.  The first wins, because taking
    // the last would let a second boundary or charset appended to a header
    // override the first -- that is a way to smuggle a parameter past
    // something that read the header before this did, not a parse.
    ok("the first wins", param("application/x; a=1; a=2", "a") == "1",
       param("application/x; a=1; a=2", "a"));

    ok("including a second boundary",
       param("multipart/mixed; boundary=real; boundary=fake", "boundary")
       == "real");
}

static void rfc2231() {
    std::cout << "\nRFC 2231, which is how a filename with an accent arrives:\n";

    {
        const content_type c =
            content_type::parse("application/pdf; filename*=UTF-8''na%C3%AFve.pdf");

        ok("an extended value is decoded",
           c.get("filename") == "na\xc3\xafve.pdf", c.get("filename"));
        ok("and the charset comes with it",
           c.charset_of("filename") == "utf-8", c.charset_of("filename"));
    }

    {
        // Outlook splits long parameters across numbered sections.
        const content_type c =
            content_type::parse("application/pdf; filename*0=\"a long \"; "
                                "filename*1=\"name.pdf\"");

        ok("sections are joined", c.get("filename") == "a long name.pdf",
           c.get("filename"));
    }

    {
        // And both at once, which is the form that actually turns up.
        const content_type c =
            content_type::parse("application/pdf; filename*0*=UTF-8''%E2%98%83; "
                                "filename*1=.txt");

        ok("an extended first section and a plain second",
           c.get("filename") == "\xe2\x98\x83.txt", c.get("filename"));
        ok("with the charset from the first",
           c.charset_of("filename") == "utf-8");
    }

    {
        // Out of order in the header, in order in the value.
        const content_type c =
            content_type::parse("application/pdf; filename*1=b; filename*0=a");

        ok("sections join in numeric order, not header order",
           c.get("filename") == "ab", c.get("filename"));
    }

    // A "%" that is not an escape is a "%".  Dropping it would lose a
    // character out of a filename because a sender wrote "100%.txt".
    ok("a stray percent survives",
       param("text/plain; name=100%.txt", "name") == "100%.txt",
       param("text/plain; name=100%.txt", "name"));

    ok("a plain parameter has no charset",
       content_type::parse("text/plain; charset=utf-8").charset_of("charset").empty());
}

static void writing_it_back_out() {
    std::cout << "\nround trip:\n";

    for(const char* s : { "text/plain",
                          "text/plain; charset=utf-8",
                          "multipart/mixed; boundary=\"a;b\"",
                          "application/x; name=\"say \\\" hi\"",
                          "application/pdf; filename*=UTF-8''na%C3%AFve.pdf" }) {
        const content_type a = content_type::parse(s);
        std::string why;
        bool same = false;

        try {
            const content_type b = content_type::parse(a.str());

            same = b.essence() == a.essence() &&
                   b.parameters().size() == a.parameters().size();

            for(std::size_t i = 0; same && i < a.parameters().size(); i++) {
                same = b.parameters()[i].name    == a.parameters()[i].name &&
                       b.parameters()[i].value   == a.parameters()[i].value &&
                       b.parameters()[i].charset == a.parameters()[i].charset;
            }
        }
        catch(content_type::exception& e) { why = e.what(); }

        ok(s, same, why.empty() ? a.str() : why);
    }

    // The reason the last of those needs RFC 2231 on the way out as well as
    // on the way in: a parameter value is ASCII.  qtext stops at %d126, so a
    // filename written back as "naïve.pdf" produces a header this very parser
    // refuses, and the round trip above is what catches that.
    ok("a non-ASCII value goes back out extended",
       content_type::parse("application/pdf; filename*=UTF-8''na%C3%AFve.pdf")
           .str().find("filename*=") != std::string::npos);
}

static void disposition() {
    std::cout << "\nContent-Disposition, RFC 2183:\n";

    const content_type d =
        content_type::parse_disposition("attachment; filename=\"a b.txt\"");

    ok("the disposition type", d.type() == "attachment");
    ok("and no subtype",       d.subtype().empty());
    ok("essence is just the type", d.essence() == "attachment", d.essence());
    ok("the filename",         d.get("filename") == "a b.txt", d.get("filename"));

    ok("inline with no parameters",
       content_type::parse_disposition("inline").type() == "inline");

    // The same grammar, so the same things work.
    ok("and RFC 2231 applies here too",
       content_type::parse_disposition("attachment; filename*=UTF-8''%E2%98%83")
           .get("filename") == "\xe2\x98\x83");
}

static void is_compares_components() {
    std::cout << "\nis():\n";

    const content_type c = content_type::parse("text/plain; charset=utf-8");

    ok("the type",            c.is("text"));
    ok("type and subtype",    c.is("text", "plain"));
    ok("case insensitively",  c.is("TEXT", "Plain"));
    ok("and not a near miss", !c.is("tex") && !c.is("text", "html"));

    // Email::is() was find() != npos over the whole lowercased header.
    jlib::net::Email latex("Content-Type: application/x-latext\r\n\r\nbody\r\n");

    ok("application/x-latext is not text", !latex.is("text"));

    // And this one is the reason it matters: the boundary is chosen by the
    // sender, so the old test could be made to say anything.
    jlib::net::Email tricky(
        "Content-Type: multipart/mixed; boundary=\"text/html\"\r\n\r\nbody\r\n");

    ok("a boundary containing a type does not make it that type",
       !tricky.is("text") && !tricky.is("text/html"));
    ok("and it is still multipart", tricky.is("multipart"));
}

static void splitting_a_body() {
    std::cout << "\nsplit_multipart, RFC 2046 5.1.1:\n";

    {
        const std::string body =
            "preamble, which is discarded\r\n"
            "--B\r\n"
            "one\r\n"
            "--B\r\n"
            "two\r\n"
            "--B--\r\n"
            "epilogue, also discarded";

        const std::vector<std::string> p = split_multipart(body, "B");

        ok("two parts", p.size() == 2, std::to_string(p.size()));

        if(p.size() == 2) {
            // The CRLF before a delimiter belongs to the delimiter, so a part
            // does not come back with a blank line stuck on the end.
            ok("without the delimiter's line break",
               p[0] == "one" && p[1] == "two",
               show(p[0]) + " | " + show(p[1]));
        }
    }

    {
        // A boundary only counts at the start of a line.
        const std::vector<std::string> p =
            split_multipart("--B\r\nsay --B please\r\n--B--\r\n", "B");

        ok("a boundary inside a line is content", p.size() == 1 &&
           p[0] == "say --B please", p.empty() ? "" : show(p[0]));
    }

    {
        // Transport padding, RFC 2046 5.1.1.
        const std::vector<std::string> p =
            split_multipart("--B  \r\none\r\n--B--  \r\n", "B");

        ok("whitespace after a boundary is skipped",
           p.size() == 1 && p[0] == "one", p.empty() ? "" : show(p[0]));
    }

    {
        // A message that has been through a file no longer has its CRLFs and
        // is not thereby a different message.
        const std::vector<std::string> p = split_multipart("--B\none\n--B--\n", "B");

        ok("bare LF works too", p.size() == 1 && p[0] == "one",
           p.empty() ? "" : show(p[0]));
    }

    {
        const std::vector<std::string> p =
            split_multipart("--B\r\none\r\n--B\r\ntruncated", "B");

        ok("a truncated body yields the parts that were terminated",
           p.size() == 1 && p[0] == "one", std::to_string(p.size()));
    }

    ok("no boundary in the body at all",
       split_multipart("just a body\r\n", "B").empty());
    ok("and an empty boundary is refused rather than matching everything",
       split_multipart("--\r\na\r\n----\r\n", "").empty());

    {
        // An empty part is a part.  A message whose first section is empty is
        // unusual and legal.
        const std::vector<std::string> p =
            split_multipart("--B\r\n\r\n--B\r\nsecond\r\n--B--\r\n", "B");

        ok("an empty part still counts", p.size() == 2 && p[0].empty(),
           std::to_string(p.size()));
    }
}

static void a_whole_message() {
    std::cout << "\nend to end:\n";

    // The boundary has a semicolon in it, which is what made this worth
    // writing: every step below used to go wrong at the first one.
    const std::string raw =
        "From: Joe <joe@x.com>\r\n"
        "Content-Type: multipart/mixed; boundary=\"a;b\"\r\n"
        "\r\n"
        "--a;b\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "hello\r\n"
        "--a;b\r\n"
        "Content-Type: application/pdf; name=\"report.pdf\"\r\n"
        "Content-Disposition: attachment; filename*=UTF-8''na%C3%AFve.pdf\r\n"
        "\r\n"
        "%PDF-1.4\r\n"
        "--a;b--\r\n";

    jlib::net::Email e(raw);

    ok("it is multipart",   e.is("multipart/mixed"));
    ok("with two parts",    e.attach().size() == 2,
       std::to_string(e.attach().size()));

    if(e.attach().size() == 2) {
        ok("the first is text",  e.attach()[0].is("text/plain"));
        ok("the second is a PDF", e.attach()[1].is("application/pdf"));

        // Content-Type's name and Content-Disposition's filename are two
        // different parameters in two different headers, and a client wants
        // whichever it can get.
        ok("its Content-Type name",
           e.attach()[1].get_name() == "report.pdf",
           e.attach()[1].get_name());
        ok("and its RFC 2231 filename",
           e.attach()[1].get_filename() == "na\xc3\xafve.pdf",
           e.attach()[1].get_filename());
    }

    // get_filename() falls back to Content-Type's name, which is new: plenty
    // of senders put the name there and nowhere else, and an attachment shown
    // with no name is the whole of what the function is for.
    jlib::net::Email named(
        "Content-Type: application/pdf; name=\"only-here.pdf\"\r\n\r\nx\r\n");

    ok("get_filename falls back to the Content-Type name",
       named.get_filename() == "only-here.pdf", named.get_filename());
}

static void the_grammar_itself() {
    std::cout << "\nthe grammar:\n";

    using namespace jlib::util::abnf;

    bool built = false;
    std::string why;

    try {
        grammar g = compile(std::string(jlib::util::rfc5322::LEXICAL) +
                            jlib::util::rfc2045::CONTENT);
        g.check();
        built = true;
    }
    catch(exception& e) { why = e.what(); }

    ok("it compiles and checks", built, why);

    // tchar is the one place in rfc2045.hh where the RFC states an exclusion
    // in prose and the grammar states ranges, so a reader cannot check it by
    // comparing.  Every excluded character, spelled out: SPACE, and RFC 2045
    // 5.1's tspecials.
    for(char c : std::string(" ()<>@,;:\\\"/[]?=")) {
        const std::string s = std::string("text/pl") + c + "ain";

        ok(std::string("tchar excludes '") + c + "'", !content_type::valid(s));
    }

    // And a sample of what it does allow, which is everything else printable.
    ok("and allows the rest",
       content_type::valid("application/x!#$%&'*+-.^_`{|}~123ABCabc"));
}

int main() {
    std::cout << std::unitbuf;

    reading_a_content_type();
    the_boundary_that_used_to_break_it();
    repeated_parameters();
    rfc2231();
    writing_it_back_out();
    disposition();
    is_compares_components();
    splitting_a_body();
    a_whole_message();
    the_grammar_itself();

    // What a green run does NOT establish.
    //
    // Not MIME conformance.  It establishes that the grammar as transcribed in
    // rfc2045.hh accepts and rejects this corpus.  RFC 2045 writes its grammar
    // in the RFC 822 style with prose exclusions rather than in ABNF, so
    // tchar had to be worked out by hand; the section above checks it
    // character by character, which is the best that can be done short of
    // checking it against the document by eye.
    //
    // Not RFC 2047.  An encoded word in a parameter comes back as the literal
    // "=?utf-8?q?...?=" -- RFC 2231 is what replaced that for parameters and
    // RFC 2231 is here, but a sender using the older form gets no help.
    //
    // Not transcoding.  charset_of() names the encoding an RFC 2231 value is
    // in and get() hands back the octets; nothing converts them, and a caller
    // that ignores the charset and assumes UTF-8 will be right almost always
    // and silently wrong the rest of the time.
    //
    // Not the rest of Email.  Content-Transfer-Encoding is still matched with
    // find("BASE64") != npos over an uppercased header, so a message declaring
    // "x-not-base64" is decoded as base64.  Same bug class, and it is small
    // enough that it belongs with whatever touches that path next rather than
    // here.
    //
    // Not boundary validity.  RFC 2046 5.1.1 bounds a boundary to 70
    // characters from a restricted set and forbids a trailing space; nothing
    // here checks that, because a sender who breaks the rule still means the
    // string they wrote and refusing the message would help nobody.
    return failures ? 1 : 0;
}
