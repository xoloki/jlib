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

// Reading ABNF text, which is the point of the whole exercise: the grammars
// in the RFCs stop being something to transcribe and become something to
// paste.
//
// The last section is the one worth reading.  The reader is bootstrapped --
// RFC 5234's own grammar, written once by hand in combinators -- so it can be
// handed the text of RFC 5234 section 4 and asked to produce a reader for
// ABNF.  That result then parses the same text, parses its own serialization,
// and compiles to a fixed point.

#include <jlib/util/abnf.hh>

#include <iostream>
#include <string>

using namespace jlib::util::abnf;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Does the named rule of this grammar match the whole of s? */
static bool matches(grammar& g, const std::string& name, const std::string& s) {
    options o;
    o.captures = options::capture_policy::none;

    return static_cast<bool>(g.at(name).try_parse(s, o));
}

static void a_grammar_read_from_text_does_what_the_text_says() {
    std::cout << "reading a grammar:\n";

    grammar g = compile(
        "addr-spec  = local-part \"@\" domain\r\n"
        "local-part = 1*atext\r\n"
        "domain     = 1*atext *(\".\" 1*atext)\r\n"
        "atext      = ALPHA / DIGIT / %x2B / %x2D / %x5F\r\n");

    ok("the rules are there, in order",
       g.rules().size() == 4 && g.rules()[0] == "addr-spec" &&
       g.rules()[3] == "atext");

    ok("and it parses",       matches(g, "addr-spec", "joe@example.com"));
    ok("including a plus",    matches(g, "addr-spec", "joe+tag@example.com"));
    ok("a subdomain",         matches(g, "addr-spec", "a@b.c.d"));
    ok("no local part",      !matches(g, "addr-spec", "@example.com"));
    ok("no domain",          !matches(g, "addr-spec", "joe@"));
    ok("a space",            !matches(g, "addr-spec", "a b@c"));

    // Core rules are predefined, which is why ALPHA and DIGIT above needed no
    // declaration -- and they do not clutter the grammar afterwards.
    ok("core rules do not show up as the grammar's own",
       g.rules().size() == 4);
}

static void the_pieces_of_the_notation() {
    std::cout << "\nthe notation:\n";

    {
        // RFC 5234 2.3: a quoted string is case INsensitive.  That is the
        // opposite of lit(), and the divergence is deliberate -- a pasted
        // grammar has to mean what the RFC says it means.
        grammar g = compile("greeting = \"HELO\"\r\n");

        ok("a quoted string is case insensitive",
           matches(g, "greeting", "helo") && matches(g, "greeting", "HeLo"));
    }

    {
        // RFC 7405 is how to ask for the other thing.
        grammar g = compile("greeting = %s\"HELO\"\r\n");

        ok("%s makes it case sensitive",
           matches(g, "greeting", "HELO") && !matches(g, "greeting", "helo"));

        grammar h = compile("greeting = %i\"HELO\"\r\n");
        ok("and %i says so explicitly",  matches(h, "greeting", "helo"));
    }

    {
        grammar g = compile(
            "a = %x41\r\n"        // one octet, hex
            "b = %d66\r\n"        // decimal
            "c = %b1000011\r\n"   // binary
            "d = %x44-46\r\n"     // a range
            "e = %x47.48\r\n");   // a sequence

        ok("hex",      matches(g, "a", "A") && !matches(g, "a", "B"));
        ok("decimal",  matches(g, "b", "B"));
        ok("binary",   matches(g, "c", "C"));
        ok("a range",  matches(g, "d", "D") && matches(g, "d", "F") &&
                      !matches(g, "d", "G"));
        ok("a sequence of octets", matches(g, "e", "GH"));
    }

    {
        grammar g = compile(
            "a = 3\"x\"\r\n"
            "b = *\"x\"\r\n"
            "c = 1*\"x\"\r\n"
            "d = 2*4\"x\"\r\n"
            "e = *3\"x\"\r\n");

        ok("exactly n", matches(g, "a", "xxx") && !matches(g, "a", "xx"));
        ok("zero or more", matches(g, "b", "") && matches(g, "b", "xxxx"));
        ok("one or more", !matches(g, "c", "") && matches(g, "c", "x"));
        ok("between n and m",
           !matches(g, "d", "x") && matches(g, "d", "xx") &&
            matches(g, "d", "xxxx") && !matches(g, "d", "xxxxx"));
        ok("at most m", matches(g, "e", "") && !matches(g, "e", "xxxx"));
    }

    {
        grammar g = compile(
            "a = (\"x\" / \"y\") \"z\"\r\n"
            "b = [\"x\"] \"z\"\r\n");

        ok("a group",  matches(g, "a", "xz") && matches(g, "a", "yz") &&
                      !matches(g, "a", "z"));
        ok("an option", matches(g, "b", "xz") && matches(g, "b", "z"));
    }

    {
        grammar g = compile(
            "; a comment on its own line\r\n"
            "a = \"x\"   ; and one after a rule\r\n"
            "\r\n"
            "b = \"y\"\r\n");

        ok("comments and blank lines are skipped",
           g.rules().size() == 2 && matches(g, "a", "x") && matches(g, "b", "y"));
    }

    {
        // A rule continues on the next line when that line is indented --
        // c-wsp being WSP or (c-nl WSP).
        grammar g = compile(
            "a = \"x\"\r\n"
            "    \"y\"\r\n"
            "    \"z\"\r\n");

        ok("a rule folded over three lines", matches(g, "a", "xyz"));
    }
}

static void incremental_alternatives() {
    std::cout << "\nABNF's \"=/\":\n";

    grammar g = compile(
        "atext = ALPHA / DIGIT\r\n"
        "atext =/ \"+\"\r\n"
        "atext =/ \"-\"\r\n");

    ok("the original alternatives",  matches(g, "atext", "q"));
    ok("and each addition",          matches(g, "atext", "+") &&
                                     matches(g, "atext", "-"));
    ok("and nothing else",          !matches(g, "atext", "@"));

    // "=/" before "=" in the bootstrap, or ordered choice takes the "=" and
    // leaves a stray "/" for the alternation to choke on.
    ok("it is one rule, not three", g.rules().size() == 1);
}

static void indentation_and_line_endings() {
    std::cout << "\npasting from an RFC:\n";

    // An RFC prints its grammar indented, and rulelist wants a rulename at
    // column zero.
    grammar g = compile(
        "   a = \"x\" b\r\n"
        "   b = \"y\"\r\n");

    ok("indented text compiles", matches(g, "a", "xy"));

    ok("dedent keeps relative indentation",
       dedent("   a = \"x\"\n       \"y\"\n") == "a = \"x\"\n    \"y\"\n");

    ok("and does nothing when there is none",
       dedent("a = \"x\"\n") == "a = \"x\"\n");

    // Text pasted from a browser has bare LFs where the RFC says CRLF.
    grammar lf = compile("a = \"x\"\n");
    ok("bare LF is accepted", matches(lf, "a", "x"));

    {
        compile_options o;
        o.allow_bare_lf = false;

        bool threw = false;
        try { compile("a = \"x\"\n", o); }
        catch(grammar_error&) { threw = true; }

        ok("unless asked to be strict about it", threw);
    }
}

static void prose_is_not_a_grammar() {
    std::cout << "\nprose-val:\n";

    // <...> means "described in words, elsewhere".  A grammar containing one
    // still compiles, because refusing two hundred rules over one obsolete
    // production would defeat the purpose of pasting them in at all.
    grammar g = compile(
        "a = \"x\" / b\r\n"
        "b = <a mailbox, as RFC 822 defines one>\r\n");

    ok("a grammar with prose in it compiles", g.rules().size() == 2);
    ok("and the part that is not prose works", matches(g, "a", "x"));

    ok("and it says up front which rules are only prose",
       g.prose_rules().size() == 1 && g.prose_rules()[0] == "b");

    bool threw = false;
    std::string msg;
    try { matches(g, "b", "anything"); }
    catch(grammar_error& e) { threw = true; msg = e.what(); }

    ok("reaching the prose is what fails", threw);
    ok("and it says which prose",
       msg.find("mailbox") != std::string::npos, msg);

    // And the hook, which is the seam this layer exists to provide.
    {
        compile_options o;
        o.prose = [](std::string_view) { return +core::DIGIT(); };

        grammar h = compile("a = <a number, in words>\r\n", o);

        ok("supplying an implementation makes it work",
           matches(h, "a", "1234"));

        ok("and then nothing is left as prose", h.prose_rules().empty());
    }
}

static void grammar_text_that_does_not_parse() {
    std::cout << "\nbad grammar text:\n";

    struct { const char* what; const char* text; } bad[] = {
        { "no equals",        "a \"x\"\r\n" },
        { "nothing after =",  "a =\r\n" },
        { "an unclosed group","a = (\"x\"\r\n" },
        { "an unclosed quote","a = \"x\r\n" },
        { "a stray slash",    "a = / \"x\"\r\n" },
        { "=/ with nothing to extend", "a =/ \"x\"\r\n" },
        { "an octet past FF", "a = %x100\r\n" },
        { "a bad digit",      "a = %b1002\r\n" },
    };

    for(const auto& b : bad) {
        bool threw = false;
        try { compile(b.text); }
        catch(grammar_error&) { threw = true; }

        ok(b.what, threw);
    }

    // A reference to something never defined is NOT an error at compile time:
    // a grammar is often assembled from several calls.  check() is where it
    // becomes one.
    bool compiled = false;
    grammar g;
    try { g = compile("a = b\r\n"); compiled = true; }
    catch(grammar_error&) { }

    ok("an undefined reference compiles", compiled);

    bool threw = false;
    try { g.check(); }
    catch(grammar_error&) { threw = true; }

    ok("and check() is what refuses it", threw);
}

// RFC 5234 section 4, verbatim but for two reorderings, each marked.
static const char* ABNF_OF_ABNF =
"rulelist       =  1*( rule / (*c-wsp c-nl) )\r\n"
"rule           =  rulename defined-as elements c-nl\r\n"
"rulename       =  ALPHA *(ALPHA / DIGIT / \"-\")\r\n"
"defined-as     =  *c-wsp (\"=/\" / \"=\") *c-wsp\r\n"
"elements       =  alternation *c-wsp\r\n"
"c-wsp          =  WSP / (c-nl WSP)\r\n"
"c-nl           =  comment / CRLF\r\n"
"comment        =  \";\" *(WSP / VCHAR) CRLF\r\n"
"alternation    =  concatenation *(*c-wsp \"/\" *c-wsp concatenation)\r\n"
"concatenation  =  repetition *(1*c-wsp repetition)\r\n"
"repetition     =  [repeat] element\r\n"
"repeat         =  (*DIGIT \"*\" *DIGIT) / 1*DIGIT\r\n"
"element        =  rulename / group / option / char-val / num-val / prose-val\r\n"
"group          =  \"(\" *c-wsp alternation *c-wsp \")\"\r\n"
"option         =  \"[\" *c-wsp alternation *c-wsp \"]\"\r\n"
"char-val       =  DQUOTE *(%x20-21 / %x23-7E) DQUOTE\r\n"
"num-val        =  \"%\" (bin-val / dec-val / hex-val)\r\n"
"bin-val        =  \"b\" 1*BIT [ 1*(\".\" 1*BIT) / (\"-\" 1*BIT) ]\r\n"
"dec-val        =  \"d\" 1*DIGIT [ 1*(\".\" 1*DIGIT) / (\"-\" 1*DIGIT) ]\r\n"
"hex-val        =  \"x\" 1*HEXDIG [ 1*(\".\" 1*HEXDIG) / (\"-\" 1*HEXDIG) ]\r\n"
"prose-val      =  \"<\" *(%x20-3D / %x3F-7E) \">\"\r\n";

static void it_reads_its_own_grammar() {
    std::cout << "\nthe fixed point:\n";

    grammar G = compile(ABNF_OF_ABNF);

    ok("RFC 5234 section 4 compiles", G.rules().size() == 21,
       std::to_string(G.rules().size()) + " rules");

    options o;
    o.captures = options::capture_policy::none;

    // The reader that was written by hand read this text; the reader the text
    // describes now reads it back.
    ok("and the result parses the text it came from",
       static_cast<bool>(G.at("rulelist").try_parse(ABNF_OF_ABNF, o)));

    const std::string once = G.to_abnf();

    ok("and its own serialization",
       static_cast<bool>(G.at("rulelist").try_parse(once, o)));

    grammar G2 = compile(once);

    ok("which compiles to the same thing again", G2.to_abnf() == once);

    // Every rule individually, which is what catches a precedence bug in
    // write(): an alternation nested in a concatenation that loses its
    // brackets still round trips as a whole if the error is symmetric.
    std::size_t differ = 0;

    for(const std::string& n : G.rules()) {
        if(G.at(n).to_abnf() != G2.at(n).to_abnf()) differ++;
    }

    ok("rule by rule as well", differ == 0,
       differ ? (std::to_string(differ) + " differ") : "");

    // The two reorderings above are not cosmetic.  As RFC 5234 writes them,
    // ordered choice takes the wrong branch -- and the grammar this produces
    // is exactly what the text says, so it inherits whatever the text did.
    {
        grammar bad = compile(
            "defined-as = *c-wsp (\"=\" / \"=/\") *c-wsp\r\n"
            "c-wsp      = WSP\r\n");

        ok("as the RFC writes defined-as, =/ cannot match",
           !matches(bad, "defined-as", "=/"));
    }
}

int main() {
    std::cout << std::unitbuf;

    a_grammar_read_from_text_does_what_the_text_says();
    the_pieces_of_the_notation();
    incremental_alternatives();
    indentation_and_line_endings();
    prose_is_not_a_grammar();
    grammar_text_that_does_not_parse();
    it_reads_its_own_grammar();

    // What a green run does NOT establish.
    //
    // Not that a grammar compiled from text behaves as RFC 5234 says it
    // should.  The engine underneath is ordered choice with possessive
    // repetition, and a compiled grammar inherits that -- which is why two of
    // RFC 5234's own productions had to be reordered above before its grammar
    // would work.  A pasted grammar means what this engine makes of it, not
    // what a different one would.
    //
    // Not that the ABNF-of-ABNF here is byte-for-byte the RFC's.  It is the
    // RFC's text with those two reorderings, and nothing checks it against the
    // published document.
    //
    // Not the external-repetition and incremental-alternative rules of RFC
    // 7405 beyond %s and %i, and not RFC 5234's own "incremental alternatives
    // must not change a rule's meaning" advice, which is guidance to a grammar
    // author and not something a reader can enforce.
    //
    // Not performance.  compile() parses with captures on and walks the tree;
    // nothing here measures either, and a grammar is compiled once.
    return failures ? 1 : 0;
}
