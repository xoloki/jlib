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

// The XML reader, which is a grammar in jlib::util::abnf and a tree.
//
// The old one was xmlpp, third-party GPL code, and was the only thing in this
// repository the author did not own outright.  This replaces it without
// reference to it, which is why the tree looks different: attributes are an
// ordered vector rather than a map so a rewrite does not rearrange them,
// parse() is a free function returning a document rather than a bool-returning
// member, and errors carry a position, which the old ones did not carry at
// all.

#include <jlib/util/xml.hh>

#include <iostream>
#include <sstream>
#include <string>

using namespace jlib::util;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static bool rejects(const std::string& doc) {
    try { xml::parse(doc); }
    catch(xml::error&) { return true; }

    return false;
}

static void elements_attributes_and_text() {
    std::cout << "reading:\n";

    xml::document r = xml::parse("<config name=\"main\" version='2'>hello</config>");

    ok("the root element",     r->name() == "config", r->name());
    ok("it is an element",     r->type() == xml::kind::element);
    ok("a double quoted value", r->get("name") == "main", r->get("name"));
    ok("a single quoted one",   r->get("version") == "2", r->get("version"));
    ok("an absent one",        r->get("nope", "fallback") == "fallback");
    ok("has()",                r->has("name") && !r->has("nope"));
    ok("the text",             r->text_content() == "hello", r->text_content());

    xml::document nested = xml::parse("<a><b>one</b><c>two</c><b>three</b></a>");

    ok("children are found by name",
       nested->first("b") && nested->first("b")->text_content() == "one");

    ok("and all of them",      nested->all("b").size() == 2);
    ok("in document order",    nested->all("b")[1]->text_content() == "three");
    ok("text_content gathers descendants",
       nested->text_content() == "onetwothree", nested->text_content());
}

static void attributes_keep_their_order() {
    std::cout << "\nattributes:\n";

    // Order is why these are a vector and not a map: a map would rewrite the
    // document with them rearranged, which turns editing a config file into a
    // diff against itself.
    xml::document r = xml::parse("<a z=\"1\" m=\"2\" a=\"3\"/>");

    ok("in the order written",
       r->attributes()[0].first == "z" && r->attributes()[1].first == "m" &&
       r->attributes()[2].first == "a");

    ok("and they survive a rewrite in that order",
       r->str() == "<a z=\"1\" m=\"2\" a=\"3\"/>", r->str());
}

static void a_repeated_attribute_is_not_xml() {
    std::cout << "\na repeated attribute:\n";

    // XML 1.0 3.1, Unique Att Spec: "An attribute name MUST NOT appear more
    // than once in the same start-tag or empty-element tag", and a violation
    // is a fatal error a conforming processor must report.  An earlier version
    // of this parser kept both, on the theory that a caller should be told
    // rather than have one dropped silently.  That was wrong in a way worth
    // recording: it accepted documents that no other parser will, so a config
    // file jlib read happily would fail everywhere else.
    bool threw = false;
    std::string msg;

    try { xml::parse("<a m=\"2\" x=\"1\" m=\"4\"/>"); }
    catch(xml::error& e) { threw = true; msg = e.what(); }

    ok("it is rejected", threw);
    ok("and named",      msg.find("\"m\"") != std::string::npos, msg);

    ok("in a child too", [] {
        try { xml::parse("<a><b p=\"1\" p=\"2\"/></a>"); }
        catch(xml::error&) { return true; }
        return false;
    }());

    // Same name, different element, is fine.
    ok("but the same name on two elements is fine",
       xml::parse("<a m=\"1\"><b m=\"2\"/></a>")->first("b")->get("m") == "2");
}

static void setting_an_attribute_means_setting_it() {
    std::cout << "\nset and remove:\n";

    xml::document r = xml::parse("<a x=\"1\" y=\"2\"/>");

    r->set("y", "changed");
    ok("set replaces an existing one", r->get("y") == "changed", r->get("y"));
    ok("without adding another",       r->attributes().size() == 2);
    ok("and in place, not at the end", r->attributes()[1].first == "y",
       r->attributes()[1].first);

    r->set("z", "new");
    ok("set adds one that is not there", r->get("z") == "new");
    ok("at the end",                     r->attributes()[2].first == "z");

    ok("remove takes it",              r->remove("y") == 1);
    ok("leaving the others",           r->attributes().size() == 2 &&
                                       r->has("x") && r->has("z"));
    ok("and removing nothing says so", r->remove("nope") == 0);

    // set() is the only way to add one, so a tree built by hand cannot hold a
    // duplicate any more than a parsed one can.
    r->set("x", "a"); r->set("x", "b"); r->set("x", "c");
    ok("setting the same name repeatedly keeps one",
       r->attributes().size() == 2 && r->get("x") == "c");
}

static void end_tags_have_to_match() {
    std::cout << "\nend tags, which the grammar itself enforces:\n";

    ok("matching",        xml::parse("<b>x</b>")->name() == "b");
    ok("mismatched",      rejects("<b>x</i>"));
    ok("unclosed",        rejects("<b>x"));
    ok("stray close",     rejects("</b>"));

    // The nesting case, which is what needed abnf's backref to become
    // scope-aware: name is one node shared by every level of the recursion, so
    // before that fix the inner close consumed the outer one's identity.
    ok("nested elements close in the right order",
       xml::parse("<a><b><c>x</c></b></a>")->first("b")->first("c")
           ->text_content() == "x");

    ok("and the outer tag still has to match", rejects("<a><b>x</b></c>"));
}

static void entities() {
    std::cout << "\nreferences:\n";

    xml::document r = xml::parse("<a t=\"&lt;q&gt;\">x &amp; y</a>");

    ok("decoded in text",   r->text_content() == "x & y", r->text_content());
    ok("and in attributes", r->get("t") == "<q>", r->get("t"));
    ok("and encoded again", r->str() == "<a t=\"&lt;q&gt;\">x &amp; y</a>",
       r->str());

    // All five, not the three the generic codec in util.hh knows.
    xml::document five = xml::parse("<a>&amp;&lt;&gt;&apos;&quot;</a>");
    ok("all five predefined entities", five->text_content() == "&<>'\"",
       five->text_content());

    // Numeric character references, decimal and hex.
    ok("a decimal reference", xml::parse("<a>&#65;</a>")->text_content() == "A");
    ok("a hex reference",     xml::parse("<a>&#x41;</a>")->text_content() == "A");
    ok("and an upper case x", xml::parse("<a>&#X41;</a>")->text_content() == "A");

    ok("one outside ASCII becomes UTF-8",
       xml::parse("<a>&#233;</a>")->text_content() == "\xc3\xa9",
       xml::parse("<a>&#233;</a>")->text_content());

    // XML 1.0 2.2: a reference may only name a character the document could
    // have held anyway, so this is not a way to smuggle a NUL in.
    ok("&#0; is refused",       rejects("<a>&#0;</a>"));
    ok("and one past Unicode",  rejects("<a>&#x110000;</a>"));
    ok("and a lone surrogate",  rejects("<a>&#xD800;</a>"));

    // Without a DTD there is nothing that could declare one, so an unknown
    // reference is a well-formedness error rather than text.  Passing it
    // through would make the document say something other than it does.
    ok("an unknown entity is refused", rejects("<a>&nbsp;</a>"));
    ok("and a bare ampersand",         rejects("<a>a & b</a>"));

    // The writer had this wrong: a quote in an attribute was written out
    // unescaped, producing <a x="say "hi""/>, which this parser could not
    // read back.
    xml::document q = xml::node::element("a");
    q->set("x", "say \"hi\"");
    ok("a quote in an attribute is escaped",
       q->str() == "<a x=\"say &quot;hi&quot;\"/>", q->str());
    ok("and survives a round trip",
       xml::parse(q->str())->get("x") == "say \"hi\"");
}

static void cdata_sections() {
    std::cout << "\nCDATA:\n";

    xml::document r = xml::parse("<a><![CDATA[x < y & z]]></a>");

    ok("the content is literal", r->text_content() == "x < y & z",
       r->text_content());

    // Written back as a section rather than as a run of escapes.  Same reason
    // <a/> is not rewritten to <a></a>: it means the same thing, and changing
    // it is a change to somebody's file they did not ask for.
    ok("and it is written back as one",
       r->str() == "<a><![CDATA[x < y & z]]></a>", r->str());

    ok("markup inside is not markup",
       xml::parse("<a><![CDATA[<b>not an element</b>]]></a>")->children().size() == 1);

    ok("an empty one",     xml::parse("<a><![CDATA[]]></a>")->text_content() == "");
    ok("an unclosed one",  rejects("<a><![CDATA[x</a>"));

    ok("beside ordinary text",
       xml::parse("<a>one <![CDATA[two]]> three</a>")->text_content() ==
       "one two three");
}

static void comments_and_the_processing_instruction() {
    std::cout << "\ncomments and <?...?>:\n";

    xml::document r = xml::parse("<a><!-- gone --><b/></a>");

    ok("a comment is skipped",  r->children().size() == 1);
    ok("leaving the element",   r->first("b") != 0);

    ok("one before the root too",
       xml::parse("<!-- hi --><a/>")->name() == "a");

    ok("a processing instruction is accepted",
       xml::parse("<?xml version=\"1.0\"?><a/>")->name() == "a");

    ok("and one with a comment after it",
       xml::parse("<?xml version=\"1.0\"?>\n<!-- x -->\n<a/>")->name() == "a");
}

static void writing_gives_back_what_was_read() {
    std::cout << "\nround trip:\n";

    const char* docs[] = {
        "<a/>",
        "<a></a>",
        "<a b=\"c\"/>",
        "<a><b>text</b></a>",
        "<a>\n  <b x=\"1\"/>\n  <b x=\"2\"/>\n</a>",
        "<a>mixed <b>content</b> here</a>",
    };

    for(const char* d : docs) {
        const std::string back = xml::to_string(xml::parse(d));

        ok(std::string("\"") + d + "\"", back == d, back);
    }

    // The two spellings of an empty element mean the same thing and are not
    // interchanged, which is what keeps the first two cases above distinct.
    ok("<a/> stays self closing",  xml::parse("<a/>")->empty_tag());
    ok("<a></a> stays open-close", !xml::parse("<a></a>")->empty_tag());
}

static void building_a_tree_by_hand() {
    std::cout << "\nbuilding:\n";

    xml::document root = xml::node::element("addressbook");

    xml::document e = root->add(xml::node::element("address"));
    e->set("key", "joey");
    e->add(xml::node::text("Joey Yandle"));

    root->add(xml::node::element("address"))->set("key", "someone");

    ok("writes what was built",
       root->str() == "<addressbook><address key=\"joey\">Joey Yandle</address>"
                      "<address key=\"someone\"/></addressbook>", root->str());

    ok("and reads back the same", xml::to_string(xml::parse(root->str())) ==
                                  root->str());
}

static void a_document_it_cannot_read_says_where() {
    std::cout << "\nerrors:\n";

    bool threw = false;
    std::size_t line = 0, column = 0;
    std::string msg;

    try {
        xml::parse("<a>\n  <b>x</c>\n</a>");
    }
    catch(xml::error& e) {
        threw = true;
        line = e.line();
        column = e.column();
        msg = e.what();
    }

    ok("a bad document throws", threw);
    ok("on the offending line", line == 2, std::to_string(line));
    ok("and gives a column",    column > 0, std::to_string(column));

    std::cout << "     " << msg << "\n";

    ok("an empty document is an error", rejects(""));
    ok("so is text with no element",    rejects("just words"));
}

static void what_it_refuses() {
    std::cout << "\ndeliberately not supported:\n";

    // Each of these is rejected outright rather than half-read.  That is the
    // point: a document this cannot represent should fail loudly, not parse
    // into something that quietly means something else.
    ok("a DOCTYPE",          rejects("<!DOCTYPE a><a/>"));
    ok("an entity declaration", rejects("<!DOCTYPE a [<!ENTITY x \"y\">]><a/>"));

    // A namespace prefix is not resolved -- it is simply part of the name.
    // Accepted, but it means nothing to this parser, and a caller treating
    // <x:a> and <y:a> as the same element because the local names match will
    // be wrong.
    xml::document r = xml::parse("<x:a x:b=\"1\"/>");
    ok("a namespace prefix stays part of the name", r->name() == "x:a", r->name());
    ok("and so does an attribute's",  r->has("x:b"));
}

int main() {
    std::cout << std::unitbuf;

    try {
        elements_attributes_and_text();
        attributes_keep_their_order();
        a_repeated_attribute_is_not_xml();
        setting_an_attribute_means_setting_it();
        end_tags_have_to_match();
        entities();
        cdata_sections();
        comments_and_the_processing_instruction();
        writing_gives_back_what_was_read();
        building_a_tree_by_hand();
        a_document_it_cannot_read_says_where();
        what_it_refuses();
    }
    catch(std::exception& e) {
        std::cerr << "xml test: " << e.what() << std::endl;
        return 1;
    }

    // What a green run does NOT establish.
    //
    // Not that this reads XML.  It reads the subset listed in xml.hh, and
    // what_it_refuses() names the parts it does not: no DTD or entity
    // declarations, no namespace resolution, and no encoding handling at all
    // -- the encoding attribute of a processing instruction is parsed and then
    // ignored, so a UTF-16 document is read as bytes and will not work, even
    // though XML 1.0 4.3.3 requires a processor to handle it.
    //
    // Nor is the input validated as UTF-8.  A numeric character reference is
    // checked against the Char production, because that costs six comparisons
    // -- but a raw invalid byte in the document is passed straight through,
    // and Name is matched over ASCII rather than over the Unicode ranges the
    // specification gives.  So &#xD800; is refused and the same codepoint
    // written directly is not, which is an inconsistency worth knowing about
    // rather than a line that has been drawn deliberately.
    //
    // Not conformance to the XML specification.  There is no test here derived
    // from the W3C test suite, and the grammar was written from the shape of
    // the documents this library actually reads rather than from the
    // specification's productions.
    //
    // Not that the processing instruction survives a round trip.  parse()
    // returns the root element and drops it, which is why the round-trip
    // section uses documents that do not have one.
    //
    // Not anything about very large or deeply nested documents beyond what
    // abnf's own depth and step guards provide, which are tested there rather
    // than here.
    return failures ? 1 : 0;
}
