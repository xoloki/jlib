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
// ordered vector rather than a map, parse() is a free function returning a
// node rather than a bool-returning member, and errors carry a position, which
// the old ones did not carry at all.

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

    xml::ptr r = xml::parse("<config name=\"main\" version='2'>hello</config>");

    ok("the root element",     r->name() == "config", r->name());
    ok("it is an element",     r->type() == xml::kind::element);
    ok("a double quoted value", r->get("name") == "main", r->get("name"));
    ok("a single quoted one",   r->get("version") == "2", r->get("version"));
    ok("an absent one",        r->get("nope", "fallback") == "fallback");
    ok("has()",                r->has("name") && !r->has("nope"));
    ok("the text",             r->text_content() == "hello", r->text_content());

    xml::ptr nested = xml::parse("<a><b>one</b><c>two</c><b>three</b></a>");

    ok("children are found by name",
       nested->first("b") && nested->first("b")->text_content() == "one");

    ok("and all of them",      nested->all("b").size() == 2);
    ok("in document order",    nested->all("b")[1]->text_content() == "three");
    ok("text_content gathers descendants",
       nested->text_content() == "onetwothree", nested->text_content());
}

static void attributes_keep_their_order_and_their_duplicates() {
    std::cout << "\nattributes:\n";

    // A map would reorder these and silently drop one.  Both matter: the
    // reorder makes a rewritten file diff against itself, and the drop hides
    // a malformed document from the caller instead of letting it decide.
    xml::ptr r = xml::parse("<a z=\"1\" m=\"2\" a=\"3\" m=\"4\"/>");

    ok("all four are kept", r->attributes().size() == 4,
       std::to_string(r->attributes().size()));

    ok("in the order written",
       r->attributes()[0].first == "z" && r->attributes()[1].first == "m" &&
       r->attributes()[2].first == "a" && r->attributes()[3].first == "m");

    ok("and get() returns the first", r->get("m") == "2", r->get("m"));
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
    std::cout << "\nthe three predefined entities:\n";

    xml::ptr r = xml::parse("<a t=\"&lt;q&gt;\">x &amp; y</a>");

    ok("decoded in text",      r->text_content() == "x & y", r->text_content());
    ok("and in attributes",    r->get("t") == "<q>", r->get("t"));
    ok("and encoded on the way out",
       r->str() == "<a t=\"&lt;q&gt;\">x &amp; y</a>", r->str());

    // Not supported, and it passes through rather than being decoded.  Stated
    // here so nobody assumes otherwise from a green run.
    xml::ptr n = xml::parse("<a>&#65;</a>");
    ok("a numeric reference is NOT decoded", n->text_content() == "&#65;",
       n->text_content());
}

static void comments_and_the_processing_instruction() {
    std::cout << "\ncomments and <?...?>:\n";

    xml::ptr r = xml::parse("<a><!-- gone --><b/></a>");

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

    xml::ptr root = xml::node::element("addressbook");

    xml::ptr e = root->add(xml::node::element("address"));
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
    ok("a DOCTYPE",   rejects("<!DOCTYPE a><a/>"));
    ok("a CDATA section", rejects("<a><![CDATA[x]]></a>"));

    // A namespace prefix is not resolved -- it is simply part of the name.
    // Accepted, but it means nothing to this parser, and a caller treating
    // <x:a> and <y:a> as the same element because the local names match will
    // be wrong.
    xml::ptr r = xml::parse("<x:a x:b=\"1\"/>");
    ok("a namespace prefix stays part of the name", r->name() == "x:a", r->name());
    ok("and so does an attribute's",  r->has("x:b"));
}

int main() {
    std::cout << std::unitbuf;

    try {
        elements_attributes_and_text();
        attributes_keep_their_order_and_their_duplicates();
        end_tags_have_to_match();
        entities();
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
    // what_it_refuses() names the parts it does not: no DTD, no CDATA
    // sections, no numeric character references, no namespace resolution, and
    // no encoding handling at all -- the encoding attribute of a processing
    // instruction is parsed and then ignored, so a UTF-16 document is read as
    // bytes and will not work.
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
