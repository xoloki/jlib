/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */

// Addresses, read against RFC 5322's own grammar.
//
// This replaces net_extract_address_{simple,mangled,noat}_test.cc and
// net_extract_address_test.cc, which between them asserted four things about
// four strings.  The first four sections below are those four tests, each
// saying what it used to assert and what changed; the rest is what could not
// be asked before there was a grammar.

#include <jlib/net/address.hh>
#include <jlib/net/rfc5322.hh>
#include <jlib/net/net.hh>

#include <jlib/util/abnf.hh>

#include <iostream>
#include <string>

using jlib::net::address;
using jlib::net::mailbox;
using jlib::net::policy;
using jlib::net::strict;
using jlib::net::lenient;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static std::string addr_of(const std::string& s) {
    try { return mailbox::parse(s, lenient()).addr().str(); }
    catch(address::exception&) { return "<threw>"; }
}

// ---------------------------------------------------- the four old tests

static void the_simple_case() {
    // was net_extract_address_simple_test.cc
    std::cout << "a bare address:\n";

    const std::string a = "joe_yandle@division-by-zero.com";

    ok("comes back as itself", addr_of(a) == a, addr_of(a));

    // What the old one could not do: say what the parts are, rather than
    // handing back a slice of the input and hoping.
    const address p = address::parse(a);

    ok("with a local part",  p.local() == "joe_yandle", p.local());
    ok("and a domain",       p.domain() == "division-by-zero.com", p.domain());
    ok("and no domain literal", !p.literal());
}

static void a_display_name() {
    // was net_extract_address_test.cc
    std::cout << "\na display name:\n";

    const std::string a = "joe_yandle@division-by-zero.com";
    const mailbox m = mailbox::parse("Joe Yandle <" + a + ">");

    ok("the address is found",  m.addr().str() == a, m.addr().str());
    ok("and so is the name",    m.name() == "Joe Yandle", m.name());

    // The old extract_address returned the address and threw the name away,
    // so a client wanting to show "Joe Yandle" had to find it again itself.
    // gtkmail did, with a regex that rejected "+" and any hyphenated domain.
    ok("and it writes back out", m.str() == "Joe Yandle <" + a + ">", m.str());
}

static void nothing_that_is_an_address() {
    // was net_extract_address_noat_test.cc, which asserted these returned "".
    std::cout << "\nnot an address:\n";

    // The empty string was the old way of saying no, and it is why
    // same_address("Joe Yandle", "Bob Smith") was true for twenty years:
    // both sides failed, both came back "", and "" == "".  These throw now.
    for(const char* s : { "no-at-sign-here", "", "Joe Yandle", "   ",
                          "joe@", "@x.com", "joe@@x.com" }) {
        bool threw = false;
        try { mailbox::parse(s, lenient()); }
        catch(address::exception&) { threw = true; }

        ok(std::string("\"") + s + "\"", threw);
    }

    // and the predicate that used to be wrong
    ok("two unreadable names are not the same person",
       !jlib::net::same_address("Joe Yandle", "Bob Smith"));
}

static void mangled() {
    // was net_extract_address_mangled_test.cc
    std::cout << "\nmangled, and the list it came in:\n";

    const std::string a = "joe_yandle@division-by-zero.com";

    // An angle bracket with nothing to match it.  The old parser handled
    // these in a function called salvage() that ran behind the caller's back;
    // it is a documented policy now, and strict() refuses them.
    ok("an unclosed bracket, leniently", addr_of("<" + a) == a);
    ok("an unopened one",                addr_of(a + ">") == a);

    ok("and strictly, neither",
       !address::valid("<" + a, strict()) && !address::valid(a + ">", strict()));

    // The 2001 test's string, and it is a good one: a leading comma, a comma
    // inside a quoted display name, and a comma inside a comment.
    const std::string s = ",  joey@dbzero.com  , \"Yandle, Joseph\" "
                          "<joey@dbzero.com>, joey@dbzero.com (Yandle, Joseph)";

    const std::vector<mailbox> got = mailbox::parse_list(s);

    ok("three addresses, not five", got.size() == 3,
       std::to_string(got.size()));

    if(got.size() == 3) {
        // source() is what was typed, which is what a recipient list shows.
        ok("the first, with its whitespace gone",
           got[0].source() == "joey@dbzero.com", got[0].source());
        ok("the second, with the comma inside its quotes",
           got[1].source() == "\"Yandle, Joseph\" <joey@dbzero.com>",
           got[1].source());
        ok("the third, with its comment",
           got[2].source() == "joey@dbzero.com (Yandle, Joseph)",
           got[2].source());

        // and the values, which are a different string
        ok("all three address the same person",
           got[0].addr().str() == "joey@dbzero.com" &&
           got[1].addr().str() == "joey@dbzero.com" &&
           got[2].addr().str() == "joey@dbzero.com");
    }
}

// ------------------------------------------------- what is new with a grammar

static void the_value_is_not_the_span() {
    std::cout << "\nthe value and the source are different strings:\n";

    // RFC 5322 puts [CFWS] *inside* atom and dot-atom, so the matched span of
    // a local part contains the spaces and comments that are not part of it.
    // substr() of the span is the obvious thing and it is wrong.
    const mailbox m = mailbox::parse("  joe . bloggs  @x.com (Joe)");

    ok("the value has the whitespace taken out",
       m.addr().local() == "joe.bloggs", m.addr().local());
    ok("and so does the domain",
       m.addr().domain() == "x.com", m.addr().domain());
    ok("the source keeps all of it",
       m.source() == "joe . bloggs  @x.com (Joe)", m.source());
    ok("and the canonical form is neither",
       m.str() == "joe.bloggs@x.com", m.str());
}

static void the_hard_ones() {
    std::cout << "\naddresses that used to be a problem:\n";

    struct { const char* in; const char* local; const char* domain; } good[] = {
        // The old extract_address stopped at any character isalnum() and a
        // handful of others rejected, so this returned "tag@x.com".
        { "joe+tag@x.com",                "joe+tag",     "x.com" },

        // Everything else atext allows and the old one did not.
        { "a!#$%&'*/?^`{|}~@x.com",       "a!#$%&'*/?^`{|}~", "x.com" },

        // A quoted local part, which the old one had no notion of.
        { "\"a,b\"@c.com",                "a,b",         "c.com" },
        { "\"say \\\" hi\"@x.com",        "say \" hi",   "x.com" },

        // A domain literal.
        { "postmaster@[123.123.123.123]", "postmaster",  "123.123.123.123" },

        // An obsolete source route: accepted, and thrown away, per RFC 5321
        // 4.1.1 -- the address is the one at the end.
        { "<@a.example,@b.example:joe@c.example>", "joe", "c.example" },

        // A period in a display name.  obs-phrase.
        { "Joe Q. Public <a@b>",          "a",           "b" },
    };

    for(const auto& g : good) {
        try {
            const mailbox m = mailbox::parse(g.in);

            ok(g.in, m.addr().local() == g.local && m.addr().domain() == g.domain,
               m.addr().local() + " @ " + m.addr().domain());
        }
        catch(address::exception& e) {
            ok(g.in, false, e.what());
        }
    }

    // The domain literal has to survive being written out, or the brackets are
    // lost and the address stops meaning what it meant.
    ok("a domain literal round trips",
       mailbox::parse("postmaster@[1.2.3.4]").str() == "postmaster@[1.2.3.4]");

    // So does a local part that needs quoting.
    ok("a quoted local part round trips",
       mailbox::parse("\"a,b\"@c.com").str() == "\"a,b\"@c.com");

    ok("and one that does not need quoting does not get quoted",
       mailbox::parse("joe+tag@x.com").str() == "joe+tag@x.com");
}

static void strict_and_obsolete() {
    std::cout << "\nsection 3 against section 4:\n";

    struct { const char* what; const char* s; } obs[] = {
        { "a period in a display name",   "Joe Q. Public <a@b>" },
        { "spaces around a dot",          "joe . bloggs@x.com" },
        { "a leading comma in a list",    ",a@b" },
        { "a source route",               "<@a.example:joe@b.example>" },
    };

    for(const auto& o : obs) {
        const bool loose = address::valid(o.s) || !mailbox::parse_list(o.s).empty();
        bool tight = true;

        try { mailbox::parse_list(o.s, strict()); }
        catch(address::exception&) { tight = false; }

        ok(std::string(o.what) + ", accepted by default", loose);
        ok(std::string(o.what) + ", refused by strict()", !tight);
    }

    // Both agree about what is simply not an address.
    ok("and neither takes a bare word",
       !address::valid("joe", strict()) && !address::valid("joe"));
}

static void groups() {
    std::cout << "\ngroups:\n";

    const std::vector<mailbox> m =
        mailbox::parse_list("Friends: a@b, Joe <c@d>;, solo@e.com");

    ok("a group's members are flattened out", m.size() == 3,
       std::to_string(m.size()));

    if(m.size() == 3) {
        ok("and remember which group they came from",
           m[0].group() == "Friends" && m[1].group() == "Friends" &&
           m[2].group().empty());
        ok("with their own display names intact", m[1].name() == "Joe");
    }

    // RFC 5322 3.4: a group with no members is legal, and this is the one
    // every mail client has seen.
    ok("an empty group is legal and has nobody in it",
       mailbox::parse_list("Undisclosed recipients:;").empty());
}

static void extending_the_grammar() {
    std::cout << "\nadding RFC 6532, which is what \"=/\" is for:\n";

    // An internationalised address is not RFC 5322 and is rejected rather
    // than mangled.  The old parser truncated at the first byte >= 0x80 --
    // isalnum() on a negative char is undefined behaviour, so what came back
    // was unpredictable rather than merely wrong.
    ok("Pel\xc3\xa9@example.com is not an RFC 5322 address",
       !address::valid("Pel\xc3\xa9@example.com"));

    // RFC 6532 is written as one incremental alternative, and so is this.
    using namespace jlib::util::abnf;

    grammar g = compile(std::string(jlib::net::rfc5322::CORE) +
                        jlib::net::rfc5322::OBSOLETE + R"ABNF(
atext           =/ UTF8-non-ascii
UTF8-non-ascii  =  UTF8-2 / UTF8-3 / UTF8-4
UTF8-2          =  %xC2-DF UTF8-tail
UTF8-3          =  %xE0 %xA0-BF UTF8-tail / %xE1-EC 2( UTF8-tail ) /
                   %xED %x80-9F UTF8-tail / %xEE-EF 2( UTF8-tail )
UTF8-4          =  %xF0 %x90-BF 2( UTF8-tail ) / %xF1-F3 3( UTF8-tail ) /
                   %xF4 %x80-8F 2( UTF8-tail )
UTF8-tail       =  %x80-BF
)ABNF");

    g.check();

    options o;
    o.captures = options::capture_policy::none;

    ok("with it, the same address parses",
       static_cast<bool>(g.at("addr-spec").try_parse("Pel\xc3\xa9@example.com", o)));

    // And it is a real UTF-8 check, not "any byte with the top bit set".
    ok("but a lone continuation byte still does not",
       !g.at("addr-spec").try_parse("Pel\x80@example.com", o));
    ok("nor an overlong encoding",
       !g.at("addr-spec").try_parse("a\xc0\x80@example.com", o));
}

static void a_hostile_header() {
    std::cout << "\ninput from a stranger:\n";

    // A From: header is written by whoever sent the message.  RFC 5322's
    // comment production is recursive, so an open parenthesis per byte is a
    // recursive descent per byte, and the answer to that has to be an
    // exception rather than a segmentation fault.
    for(int n : { 2000, 50000 }) {
        const std::string s = std::string(n, '(') + "x" + std::string(n, ')')
                            + "joe@x.com";

        std::string msg;

        try { mailbox::parse(s); }
        catch(address::exception& e) { msg = e.what(); }

        ok(std::to_string(n) + " nested comments is refused, not fatal",
           !msg.empty());

        // And refused for the right reason.  It is not that the text is not an
        // address -- it very nearly is one -- it is that the parser stopped.
        ok("  and says it gave up rather than that it disagreed",
           msg.find("gave up") != std::string::npos, msg.substr(0, 70));
    }

    // The same shape, shallow enough to be legal, still works.
    ok("a hundred deep is a perfectly good address",
       mailbox::parse(std::string(100, '(') + "x" + std::string(100, ')')
                      + "joe@x.com").addr().str() == "joe@x.com");
}

static void the_grammar_itself() {
    std::cout << "\nthe grammar:\n";

    using namespace jlib::util::abnf;

    // Both policies have to build, and check() is what says a rule is
    // referenced and never defined, or left recursive, or repeats something
    // that can match nothing.  If this ever fails it fails here rather than
    // on whatever address happened to reach the broken rule.
    for(const char* d : { jlib::net::rfc5322::STRICT,
                          jlib::net::rfc5322::OBSOLETE }) {
        bool built = false;
        std::string why;

        try {
            grammar g = compile(std::string(jlib::net::rfc5322::CORE) + d);
            g.check();
            built = true;
        }
        catch(exception& e) { why = e.what(); }

        ok(d == jlib::net::rfc5322::STRICT ? "strict compiles and checks"
                                           : "obsolete compiles and checks",
           built, why);
    }
}

int main() {
    std::cout << std::unitbuf;

    the_simple_case();
    a_display_name();
    nothing_that_is_an_address();
    mangled();
    the_value_is_not_the_span();
    the_hard_ones();
    strict_and_obsolete();
    groups();
    extending_the_grammar();
    a_hostile_header();
    the_grammar_itself();

    // What a green run does NOT establish.
    //
    // Not RFC 5322 conformance.  It establishes that the grammar *as
    // transcribed in rfc5322.hh* accepts and rejects this corpus.  Neither the
    // transcription nor the corpus has been checked against the errata, and
    // the engine underneath is PEG ordered choice with possessive repetition,
    // which is not what RFC 5234 describes -- two of the productions in
    // rfc5322.hh are reordered for exactly that reason, and each says so.
    //
    // Not deliverability.  RFC 5322 syntax is not RFC 5321 routability:
    // a@-b.example parses here and no mail server will take it.  There are no
    // length limits and no LDH hostname rules.
    //
    // Not UTF-8 handling in the default grammar.  atext is ASCII, so a byte
    // over 0x7F is rejected -- the RFC 6532 section above adds it, and that
    // grammar is built in the test and not used anywhere else.
    //
    // Not RFC 2047.  "=?utf-8?q?Joe?= <a@b>" parses, and name() comes back as
    // the literal "=?utf-8?q?Joe?=" rather than "Joe"; decoding an encoded
    // word is util::Headers' job and it is not done here.
    //
    // Not thread safety.  The grammars are function-local statics, which C++11
    // makes safe to initialise, and a grammar is read-only once check() has
    // run -- so parsing from several threads is *intended* to be safe.  It is
    // not tested, and if a memo table is ever added to abnf it must live on the
    // per-call context or that goes away silently.
    //
    // Not performance.  There is no memoization in the engine and nothing here
    // measures a parse.  Measured separately while writing this: about 20
    // microseconds for one address and 2.4 ms for a 100-address header of
    // 1988 bytes, which is linear enough that packrat has nothing to fix.
    //
    // Not a guarantee about the depth limit.  The section above shows the
    // guard firing rather than the stack running out, on this machine, with
    // this stack size.  The limit is a number in abnf::options, not a property
    // of the platform.
    return failures ? 1 : 0;
}
