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

#include <jlib/util/xml.hh>

#include <jlib/util/abnf.hh>
#include <jlib/util/util.hh>

#include <istream>
#include <ostream>
#include <sstream>

namespace jlib {
namespace util {
namespace xml {

// -------------------------------------------------------------------- error

error::error(const std::string& msg, std::size_t line, std::size_t column)
    : std::runtime_error("jlib::util::xml::error: " + msg +
                         " at line " + std::to_string(line) +
                         ", column " + std::to_string(column)),
      m_line(line),
      m_column(column)
{
}

// ---------------------------------------------------------------- escaping

namespace {

/**
 * XML's five predefined entities, plus numeric references.
 *
 * Not util::xml::encode/decode, which knows three of the five and nothing
 * about &#65;.  Escaping here is also context sensitive -- an attribute value
 * is delimited by a quote, so the quote has to go, and text is not -- which a
 * single table cannot express.
 */
std::string escape(std::string_view s, bool attribute)
{
    std::string out;
    out.reserve(s.size());

    for(char c : s) {
        switch(c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':
            // Only inside an attribute, where it would end the value.  This
            // was missing, so an attribute holding a quote was written as
            // <a x="say "hi""/> -- output this parser could not read back.
            if(attribute) out += "&quot;";
            else          out += c;
            break;
        default:   out += c;        break;
        }
    }

    return out;
}

/** XML 1.0 2.2: the codepoints a document may contain at all. */
bool is_char(unsigned long c)
{
    return c == 0x9 || c == 0xA || c == 0xD ||
           (c >= 0x20    && c <= 0xD7FF) ||
           (c >= 0xE000  && c <= 0xFFFD) ||
           (c >= 0x10000 && c <= 0x10FFFF);
}

/** Line and column of an offset, counting from one. */
std::pair<std::size_t, std::size_t> where(std::string_view in, std::size_t at)
{
    std::size_t line = 1, start = 0;

    for(std::size_t i = 0; i < at && i < in.size(); i++) {
        if(in[i] == '\n') { line++; start = i + 1; }
    }

    return std::make_pair(line, at - start + 1);
}

[[noreturn]] void fail(std::string_view input, std::size_t at,
                       const std::string& why)
{
    const std::pair<std::size_t, std::size_t> p = where(input, at);

    throw error(why, p.first, p.second);
}

void append_utf8(std::string& out, unsigned long c)
{
    if(c < 0x80) {
        out += static_cast<char>(c);
    }
    else if(c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    else if(c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

/**
 * Turn references back into the characters they stand for.
 *
 * base is where s sits in input, so a bad reference can be reported at the
 * place it actually occurs rather than at the start of the document.
 */
std::string unescape(std::string_view s, std::string_view input, std::size_t base)
{
    std::string out;
    out.reserve(s.size());

    for(std::size_t i = 0; i < s.size(); i++) {
        if(s[i] != '&') {
            out += s[i];
            continue;
        }

        const std::size_t semi = s.find(';', i);

        if(semi == std::string_view::npos) {
            fail(input, base + i, "an & that starts no reference");
        }

        const std::string_view ref = s.substr(i + 1, semi - i - 1);

        if(ref == "amp")       out += '&';
        else if(ref == "lt")   out += '<';
        else if(ref == "gt")   out += '>';
        else if(ref == "apos") out += '\'';
        else if(ref == "quot") out += '"';
        else if(!ref.empty() && ref[0] == '#') {
            const bool hex = ref.size() > 1 && (ref[1] == 'x' || ref[1] == 'X');
            const std::string_view digits = ref.substr(hex ? 2 : 1);

            if(digits.empty()) {
                fail(input, base + i, "a character reference with no digits");
            }

            unsigned long c = 0;

            for(char d : digits) {
                int v;

                if(d >= '0' && d <= '9')                v = d - '0';
                else if(hex && d >= 'a' && d <= 'f')    v = d - 'a' + 10;
                else if(hex && d >= 'A' && d <= 'F')    v = d - 'A' + 10;
                else fail(input, base + i,
                          "a character reference with \"" + std::string(1, d) +
                          "\" in it");

                c = c * (hex ? 16 : 10) + static_cast<unsigned long>(v);

                if(c > 0x10FFFF) {
                    fail(input, base + i,
                         "a character reference past the end of Unicode");
                }
            }

            // XML 1.0 2.2: a reference may only name a character the document
            // could have contained anyway, so &#0; is not a way in.
            if(!is_char(c)) {
                fail(input, base + i,
                     "character reference &" + std::string(ref) +
                     "; is not a character XML allows");
            }

            append_utf8(out, c);
        }
        else {
            // Without a DTD there is nothing that could have declared it, so
            // this is a well-formedness error rather than text.  Passing it
            // through would make the document say something else.
            fail(input, base + i,
                 "no entity named \"" + std::string(ref) + "\" is declared");
        }

        i = semi;
    }

    return out;
}

}

// --------------------------------------------------------------------- node

node::node(kind k, std::string name, std::string content)
    : m_kind(k),
      m_name(std::move(name)),
      m_content(std::move(content))
{
}

namespace {

/**
 * shared_ptr needs a public constructor to use make_shared, and node's is
 * protected on purpose -- the static makers are the only way in.  This is the
 * usual way to have both.
 */
struct makeable : public node {
    makeable(kind k, std::string name, std::string content)
        : node(k, std::move(name), std::move(content)) {}
};

}

node::ptr node::element(std::string name)
{
    return std::make_shared<makeable>(kind::element, std::move(name),
                                      std::string());
}

node::ptr node::text(std::string content, bool cdata)
{
    node::ptr n = std::make_shared<makeable>(kind::text, std::string(),
                                             std::move(content));
    n->set_cdata(cdata);

    return n;
}

node::ptr node::instruction(std::string target, std::string body)
{
    return std::make_shared<makeable>(kind::instruction, std::move(target),
                                      std::move(body));
}

bool node::has(std::string_view name) const
{
    for(const attribute& a : m_attributes) {
        if(a.first == name) return true;
    }

    return false;
}

std::string node::get(std::string_view name, const std::string& fallback) const
{
    for(const attribute& a : m_attributes) {
        if(a.first == name) return a.second;
    }

    return fallback;
}

void node::set(std::string name, std::string value)
{
    for(attribute& a : m_attributes) {
        if(a.first == name) {
            // In place: replacing must not move it to the end and reorder the
            // document.
            a.second = std::move(value);
            return;
        }
    }

    m_attributes.push_back(attribute(std::move(name), std::move(value)));
}

std::size_t node::remove(std::string_view name)
{
    const std::size_t was = m_attributes.size();

    attribute_list keep;

    for(const attribute& a : m_attributes) {
        if(a.first != name) keep.push_back(a);
    }

    m_attributes.swap(keep);

    return was - m_attributes.size();
}

node::ptr node::add(ptr child)
{
    m_children.push_back(child);

    return child;
}

node::ptr node::first(std::string_view name) const
{
    for(const ptr& c : m_children) {
        if(c->type() == kind::element && c->name() == name) return c;
    }

    return ptr();
}

node::child_list node::all(std::string_view name) const
{
    child_list out;

    for(const ptr& c : m_children) {
        if(c->type() == kind::element && c->name() == name) out.push_back(c);
    }

    return out;
}

std::string node::text_content() const
{
    if(m_kind == kind::text) return m_content;

    std::string out;

    for(const ptr& c : m_children) out += c->text_content();

    return out;
}

void node::write(std::ostream& os) const
{
    switch(m_kind) {
    case kind::text:
        if(m_cdata) os << "<![CDATA[" << m_content << "]]>";
        else        os << escape(m_content, false);
        return;

    case kind::instruction:
        os << "<?" << m_name << m_content << "?>";
        return;

    case kind::element:
        break;
    }

    os << "<" << m_name;

    for(const attribute& a : m_attributes) {
        os << " " << a.first << "=\"" << escape(a.second, true) << "\"";
    }

    if(m_children.empty() && m_empty_tag) {
        os << "/>";
        return;
    }

    os << ">";

    for(const ptr& c : m_children) c->write(os);

    os << "</" << m_name << ">";
}

std::string node::str() const
{
    std::ostringstream o;
    write(o);

    return o.str();
}

// ------------------------------------------------------------- the grammar

namespace {

using namespace jlib::util::abnf;

/**
 * XML, as much of it as this reads.
 *
 * Built once, on first use, for the reason recorded in crypt/curve.hh: a
 * namespace-scope object here would be constructed while the library loads,
 * before anything could have gone wrong in a way anyone could see.
 *
 * The end tag is backref(name) -- the grammar itself requires the close to
 * match the open, so there is no separate check to forget.  That needs the
 * scope-aware backref, since name is one node shared by every level of the
 * recursion; see the note on tracked_record in abnf.cc.
 */
class xml_grammar {
public:
    xml_grammar()
    {
        const rule ws = anyof(" \t\r\n");
        const rule name_start = core::ALPHA() | anyof("_:");
        const rule name_rest = core::ALPHA() | core::DIGIT() | anyof(".-_:");

        m_name = as("name", name_start >> *name_rest);

        // A comment is skipped entirely, not captured.  Not in the original
        // scope for this, and added anyway: a hand-edited config file with a
        // comment in it is not an exotic document, and refusing to read one
        // would be a worse failure than the feature is worth.
        const rule comment = lit("<!--") >> until(lit("-->")) >> lit("-->");

        const rule quoted =
            (core::DQUOTE() >> as("value", *anyof_but("\"")) >> core::DQUOTE()) |
            (lit("'") >> as("value", *anyof_but("'")) >> lit("'"));

        const rule attribute =
            as("attribute", +ws >> as("attribute-name", name_start >> *name_rest)
                            >> *ws >> lit("=") >> *ws >> quoted);

        const rule text = as("text", +anyof_but("<"));

        // Literal text, entities and all.  Its own capture so the writer can
        // put it back as a CDATA section rather than as a run of escapes.
        const rule cdata =
            lit("<![CDATA[") >> as("cdata", until(lit("]]>"))) >> lit("]]>");

        // element = "<" name *attribute *ws ( "/>" / ">" content "</" name ">" )
        m_grammar.define("element",
            as("element",
               lit("<") >> m_name >> *attribute >> *ws
               >> ( as("self-closing", lit("/>"))
                  | ( lit(">")
                      >> *( m_grammar["element"] | comment | cdata | text )
                      >> lit("</") >> backref(m_name) >> *ws >> lit(">") ) )));

        const rule instruction =
            as("instruction", lit("<?") >> as("target", name_start >> *name_rest)
                              >> as("body", until(lit("?>"))) >> lit("?>"));

        m_grammar.define("document",
            *ws >> -(instruction >> *ws) >> *(comment >> *ws)
                >> m_grammar["element"] >> *ws);

        m_grammar.check();
    }

    const rule document() const { return m_grammar.at("document"); }

protected:
    /** Every octet except these.  XML's negated character classes. */
    static rule anyof_but(std::string_view chars)
    {
        std::string keep;

        for(unsigned int c = 0; c < 256; c++) {
            bool skip = false;

            for(char x : chars) {
                if(static_cast<unsigned char>(x) == c) skip = true;
            }

            if(!skip) keep += static_cast<char>(c);
        }

        return anyof(keep);
    }

    mutable grammar m_grammar;
    rule m_name;
};

const xml_grammar& reader()
{
    static const xml_grammar g;

    return g;
}

// ------------------------------------------------------------- tree building

node::ptr build(const match& m, std::string_view input);

void add_children(const match& m, const node::ptr& into, std::string_view input)
{
    for(const match& c : m.children()) {
        const std::string what = c.name();

        if(what == "element") {
            into->add(build(c, input));
        }
        else if(what == "text") {
            into->add(node::text(unescape(c.str(), input, c.begin())));
        }
        else if(what == "cdata") {
            // Literal by definition: nothing inside is a reference.
            into->add(node::text(c.str(), true));
        }
    }
}

node::ptr build(const match& m, std::string_view input)
{
    const match n = m.child("name");

    node::ptr e = node::element(n ? n.str() : std::string());

    // <a/> and <a></a> both have no children; only this tells them apart.
    e->set_empty_tag(static_cast<bool>(m.child("self-closing")));

    for(const match& a : m.children()) {
        if(a.name() != "attribute") continue;

        const match an = a.child("attribute-name");
        const match av = a.child("value");

        const std::string key = an ? an.str() : std::string();

        // XML 1.0 3.1, Unique Att Spec: an attribute name must not appear more
        // than once in the same tag, and a violation is a fatal error a
        // conforming processor has to report.  Accepting one would mean
        // reading documents that no other parser will.
        if(e->has(key)) {
            const std::pair<std::size_t, std::size_t> at =
                where(input, an ? an.begin() : a.begin());

            throw error("attribute \"" + key + "\" appears more than once on <" +
                        e->name() + ">", at.first, at.second);
        }

        e->set(key, av ? unescape(av.str(), input, av.begin()) : std::string());
    }

    add_children(m, e, input);

    return e;
}

}  // namespace

// -------------------------------------------------------------------- parse

document parse(std::string_view text)
{
    const parse_result r = reader().document().try_parse(text);

    if(!r) {
        const abnf::error& e = r.why();

        // Built from the parts rather than from what(), which already
        // carries abnf's own prefix and would give a message naming two
        // libraries for one failure.
        std::string wanted;

        for(std::size_t i = 0; i < e.expected().size() && i < 3; i++) {
            wanted += (i ? " or " : "") + e.expected()[i];
        }

        throw error(wanted.empty() ? "unexpected input" : "expected " + wanted,
                    e.line(), e.column());
    }

    const match root = r.root();
    const match top = root["element"];

    if(!top) {
        throw error("no root element", 1, 1);
    }

    return build(top, text);
}

document parse(std::istream& is)
{
    std::ostringstream o;
    o << is.rdbuf();

    return parse(o.str());
}

void write(std::ostream& os, const document& root)
{
    if(root) root->write(os);
}

std::string to_string(const document& root)
{
    std::ostringstream o;
    write(o, root);

    return o.str();
}

}
}
}
