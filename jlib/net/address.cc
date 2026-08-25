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

#include <jlib/net/address.hh>
#include <jlib/net/rfc5322.hh>

#include <jlib/util/abnf.hh>

#include <set>
#include <string>

namespace jlib {
namespace net {

namespace {

using util::abnf::grammar;
using util::abnf::match;
using util::abnf::options;

grammar build(bool obsolete, bool lenient)
{
    std::string text = rfc5322::CORE;

    text += obsolete ? rfc5322::OBSOLETE : rfc5322::STRICT;
    if(lenient) text += rfc5322::LENIENT;

    grammar g = util::abnf::compile(text);

    // Up front rather than on the first parse, so a mistake in the grammar
    // text is a failure to build the grammar and not a failure to read some
    // particular address six months later.
    g.check();

    return g;
}

/**
 * The grammars, one per policy, built on first use.
 *
 * Function-local rather than namespace-scope, for the reason written up at
 * crypt/curve.hh:42: a namespace-scope object whose constructor does real work
 * runs while the shared library loads, before anything has had a chance to set
 * up, and a failure there has nowhere to report itself.  Compiling a grammar
 * allocates and can throw, which is exactly that shape.
 *
 * The C++11 guarantee makes the initialisation thread safe, and a grammar is
 * read-only once check() has run -- all parse state lives on a per-call
 * context -- so parsing from several threads is safe as well.
 */
const grammar& grammar_for(const policy& p)
{
    if(p.lenient) {
        if(p.obsolete) {
            static grammar g = build(true, true);
            return g;
        }

        static grammar g = build(false, true);
        return g;
    }

    if(p.obsolete) {
        static grammar g = build(true, false);
        return g;
    }

    static grammar g = build(false, false);
    return g;
}

/**
 * The rules whose matches are kept.
 *
 * capture_policy::named on a grammar this size records a match per character
 * -- atext fires once per octet of every atom -- so the set is spelled out.
 * Everything in it is either a container to navigate by or a value to read,
 * and because the rules in between are not recorded, what is left is a shallow
 * tree: an addr-spec's local-part is its child whichever policy matched it.
 */
const std::set<std::string>& wanted()
{
    static const std::set<std::string> s = {
        "address", "group", "mailbox", "display-name", "addr-spec",
        "local-part", "domain",
        "word", "phrase-dot", "atom-text", "qs-body", "dtext-body",
    };

    return s;
}

options parse_options()
{
    options o;

    o.captures = options::capture_policy::listed;
    o.capture_only = wanted();

    return o;
}

/**
 * Depth-first, in document order, stopping at any name in stop.
 *
 * The shape under a local-part depends on which policy matched it -- a strict
 * dot-atom puts atom-text directly there, an obs-local-part puts a word in
 * between -- and this returns the values in the same order either way.
 */
void collect(const match& m, const std::set<std::string>& stop,
             std::vector<match>& out)
{
    for(const match& c : m.children()) {
        if(stop.count(c.name())) out.push_back(c);
        else                     collect(c, stop, out);
    }
}

std::vector<match> pieces(const match& m, std::set<std::string> names)
{
    std::vector<match> out;

    collect(m, names, out);

    return out;
}

/**
 * Undo quoted-pair and folding.
 *
 * RFC 5322 3.2.2: unfolding removes the CRLF of a fold and keeps the
 * whitespace after it.  3.2.1: a quoted-pair is a backslash and the character
 * after it, and means that character.
 */
std::string unescape(std::string_view s)
{
    std::string out;

    out.reserve(s.size());

    for(std::size_t i = 0; i < s.size(); ++i) {
        if(s[i] == '\\' && i + 1 < s.size()) {
            out += s[++i];
        }
        else if(s[i] != '\r' && s[i] != '\n') {
            out += s[i];
        }
    }

    return out;
}

/** One piece of a local part, domain or display name, as its value. */
std::string value_of(const match& m)
{
    return m.name() == "atom-text" ? m.str() : unescape(m.text());
}

/** Joined with ".", which is what separates the pieces of both of them. */
std::string dotted(const match& m)
{
    std::string out;

    for(const match& p : pieces(m, {"atom-text", "qs-body", "dtext-body"})) {
        if(!out.empty()) out += ".";
        out += value_of(p);
    }

    return out;
}

/**
 * A phrase, put back together.
 *
 * Words are separated by a single space however much folding and however many
 * comments sat between them; a "." from obs-phrase attaches to the word before
 * it, so "Joe Q. Public" comes back as itself.
 */
std::string phrase_of(const match& m)
{
    std::string out;

    for(const match& p : pieces(m, {"atom-text", "qs-body", "phrase-dot"})) {
        if(p.name() == "phrase-dot") {
            out += ".";
            continue;
        }

        if(!out.empty()) out += " ";
        out += value_of(p);
    }

    return out;
}

const std::string& atext()
{
    static const std::string s =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        "!#$%&'*+-/=?^_`{|}~";

    return s;
}

std::string quote(std::string_view s)
{
    std::string out = "\"";

    for(char c : s) {
        if(c == '"' || c == '\\') out += '\\';
        out += c;
    }

    return out + "\"";
}

/** Trim the whitespace an obsolete list rule leaves on the ends of a span. */
std::string_view trim(std::string_view s)
{
    const char* ws = " \t\r\n";

    const std::size_t b = s.find_first_not_of(ws);
    if(b == s.npos) return std::string_view();

    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

/**
 * Parse the whole of s as one rule, or throw.
 *
 * abnf::error carries an offset and a rendered report already; this keeps both
 * and puts a net-shaped name on it, so a caller catching address::exception
 * does not also have to know about jlib::util::abnf.
 */
match run(const grammar& g, const std::string& rule, std::string_view s)
{
    try {
        return g.at(rule).parse(s, parse_options());
    }
    catch(util::abnf::budget_exceeded& e) {
        // Before error, which it derives from.  A parse that ran out of steps
        // or nesting is not the same answer as "that is not an address", and
        // saying so matters here: this is the shape a hostile From: header
        // takes.  Five hundred nested comments is the depth guard, and it is a
        // security control -- the alternative is a segmentation fault from a
        // header a stranger wrote.
        throw address::exception(std::string("gave up reading an address: ")
                                 + e.what(), std::string(s), e.offset());
    }
    catch(util::abnf::error& e) {
        // Not e.what().  The grammar's own message lists every terminal that
        // could have continued, which for an address is forty of them and
        // tells a caller nothing it can act on; where it stopped, and the text
        // with a caret under it, is the whole of what is useful.
        throw address::exception("not an address at column "
                                 + std::to_string(e.column()) + "\n  "
                                 + e.context_line() + "\n  "
                                 + std::string(e.column() - 1, ' ') + "^",
                                 std::string(s), e.offset());
    }
    catch(util::abnf::exception& e) {
        throw address::exception(e.what(), std::string(s), 0);
    }
}

}

// -------------------------------------------------------------- the readers

struct reader {

    static address addr_spec(const match& m)
    {
        address a;
        const match domain = m.child("domain");

        a.m_local = dotted(m.child("local-part"));
        a.m_domain = dotted(domain);
        a.m_literal = !pieces(domain, {"dtext-body"}).empty();

        return a;
    }

    static mailbox one(const match& m, const std::string& group)
    {
        mailbox b;
        const match name = m.child("display-name");

        if(name) b.m_name = phrase_of(name);

        b.m_addr = addr_spec(m.child("addr-spec"));
        b.m_group = group;

        // The span, less the whitespace an obsolete list rule leaves on its
        // ends.  Comments are inside it and stay: RFC 5322 puts [CFWS] within
        // atom, so "joe@x.com (Joe)" is all one domain's worth of match, and a
        // client showing a recipient list should show what was written.
        b.m_source = std::string(trim(m.text()));

        return b;
    }

    /** Every mailbox under m, each labelled with the group it was found in. */
    static void addresses(const match& m, std::vector<mailbox>& out)
    {
        for(const match& a : m.all("address")) {
            const match g = a.child("group");

            if(!g) {
                out.push_back(one(a.child("mailbox"), std::string()));
                continue;
            }

            // A group's own display-name is the group name, and each member
            // may carry one of its own -- which is why this is read from the
            // group rather than searched for from the address.
            const match name = g.child("display-name");
            const std::string label = name ? phrase_of(name) : std::string();

            for(const match& b : g.all("mailbox")) {
                out.push_back(one(b, label));
            }
        }
    }
};

// ---------------------------------------------------------------- exceptions

address::exception::exception(const std::string& msg, std::string text,
                              std::size_t offset)
    : std::runtime_error("jlib::net::address::exception: " + msg),
      m_text(std::move(text)),
      m_offset(offset)
{
}

// -------------------------------------------------------------------- policy

policy strict()
{
    policy p;

    p.obsolete = false;

    return p;
}

policy lenient()
{
    policy p;

    p.lenient = true;

    return p;
}

// ------------------------------------------------------------------- address

address address::parse(std::string_view s)
{
    return parse(s, policy());
}

address address::parse(std::string_view s, const policy& p)
{
    // addr-spec, not mailbox: an address is a local-part, an "@" and a domain,
    // and "Joe <a@b>" is a mailbox that has one.  Keeping the two apart is
    // what stops a display name being quietly accepted as an address.
    const match m = run(grammar_for(p), "addr-spec", s);

    return reader::addr_spec(m.child("addr-spec"));
}

bool address::valid(std::string_view s)
{
    return valid(s, policy());
}

bool address::valid(std::string_view s, const policy& p)
{
    try {
        parse(s, p);
        return true;
    }
    catch(exception&) {
        return false;
    }
}

bool address::must_quote() const
{
    if(m_local.empty()) return true;

    if(m_local.front() == '.' || m_local.back() == '.') return true;
    if(m_local.find("..") != m_local.npos) return true;

    for(char c : m_local) {
        if(c != '.' && atext().find(c) == std::string::npos) return true;
    }

    return false;
}

std::string address::str() const
{
    const std::string local = must_quote() ? quote(m_local) : m_local;

    return local + "@" + (m_literal ? "[" + m_domain + "]" : m_domain);
}

// ------------------------------------------------------------------- mailbox

mailbox mailbox::parse(std::string_view s)
{
    return parse(s, policy());
}

mailbox mailbox::parse(std::string_view s, const policy& p)
{
    const match m = run(grammar_for(p), "mailbox", s);

    return reader::one(m.child("mailbox"), std::string());
}

std::vector<mailbox> mailbox::parse_list(std::string_view s)
{
    return parse_list(s, policy());
}

std::vector<mailbox> mailbox::parse_list(std::string_view s, const policy& p)
{
    std::vector<mailbox> out;

    reader::addresses(run(grammar_for(p), "address-list", s), out);

    return out;
}

std::string mailbox::str() const
{
    if(m_name.empty()) return m_addr.str();

    bool plain = true;

    for(char c : m_name) {
        if(c != ' ' && atext().find(c) == std::string::npos) plain = false;
    }

    return (plain ? m_name : quote(m_name)) + " <" + m_addr.str() + ">";
}

}
}
