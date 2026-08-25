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

#ifndef JLIB_NET_ADDRESS_HH
#define JLIB_NET_ADDRESS_HH

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace jlib {
namespace net {

/**
 * Email addresses, parsed against RFC 5322's grammar.
 *
 * The grammar is the RFC's own ABNF, in jlib/net/rfc5322.hh, read by
 * jlib::util::abnf::compile().  Nothing here scans by hand.
 *
 * ## The two things a parsed address gives you
 *
 * A **value** and a **source**, and they are not the same string.
 *
 *     mailbox m = mailbox::parse("  joe . bloggs  @x.com (Joe)");
 *     m.addr().local()   ->  "joe.bloggs"
 *     m.addr().str()     ->  "joe.bloggs@x.com"
 *     m.source()         ->  "joe . bloggs  @x.com (Joe)"
 *
 * The value is built from the productions that carry meaning; the source is
 * what was typed.  RFC 5322 puts [CFWS] *inside* atom and dot-atom, so the
 * matched span of a local-part is full of the spaces and comments that are not
 * part of it -- taking substr() of the span, which is the obvious thing, is
 * wrong, and is most of what makes hand-rolled address parsing fail.
 *
 * Both are wanted.  A client sending mail needs the value; a client showing a
 * recipient list needs to show what the user wrote, comment and all.
 *
 * ## Policies
 *
 * Three axes, one struct.  The default accepts RFC 5322's obsolete syntax,
 * because real mail is full of it and a mail client that rejects
 * "Joe Q. Public <a@b>" is wrong about the world.
 *
 *     address::parse(s)              obsolete syntax, no repair
 *     address::parse(s, strict())    section 3 only
 *     address::parse(s, lenient())   plus unbalanced angle brackets
 *
 * lenient() is a mode the caller asks for, not a hidden fallback.  Its
 * predecessor here was a function called salvage() that ran when the parser
 * failed, so extract_address() and the parser disagreed about what an address
 * was and only one of them was documented.
 *
 * ## Failure
 *
 * Throws.  Every entry point throws address::exception on input it cannot
 * read, and none of them returns an empty string to mean "no".  That contract
 * is what let same_address("Joe Yandle", "Bob Smith") answer true for twenty
 * years: both sides failed to parse, both came back "", and "" == "".
 */

/**
 * Fills in an address and a mailbox from a parse tree.
 *
 * Declared here and defined in address.cc, so that the one thing needing to
 * write these fields can, without jlib/util/abnf.hh appearing in a header that
 * jlib/net/net.hh includes -- which would put the combinators in front of
 * every translation unit in the library.
 */
struct reader;

/** Which grammar to read with. */
struct policy {
    /** Accept RFC 5322 section 4's obsolete syntax.  On by default. */
    bool obsolete = true;

    /** Accept an angle bracket with nothing to match it. */
    bool lenient = false;
};

/** RFC 5322 section 3 only. */
policy strict();

/** Obsolete syntax, plus repair of unbalanced angle brackets. */
policy lenient();

/**
 * An addr-spec: the local-part, the "@", and the domain.
 *
 * Never a display name and never a comment -- that is mailbox.
 */
class address {
public:
    /** What could not be read, and where it stopped making sense. */
    class exception : public std::runtime_error {
    public:
        exception(const std::string& msg, std::string text, std::size_t offset);

        /** The text handed to parse(). */
        const std::string& text() const { return m_text; }

        /** How far into it the parse got, in octets. */
        std::size_t offset() const { return m_offset; }

    protected:
        std::string m_text;
        std::size_t m_offset;
    };

    address() = default;

    /** The whole of s must be one address, or this throws. */
    static address parse(std::string_view s);
    static address parse(std::string_view s, const policy& p);

    /** Would s parse?  The way to ask without catching. */
    static bool valid(std::string_view s);
    static bool valid(std::string_view s, const policy& p);

    /** The value, unquoted and with the CFWS gone: "joe.bloggs". */
    const std::string& local() const { return m_local; }

    /** The value: "x.com", or "123.123.123.123" for a domain literal. */
    const std::string& domain() const { return m_domain; }

    /** Was the domain written as [1.2.3.4] rather than as a name? */
    bool literal() const { return m_literal; }

    /**
     * Does the local part need quoting to be written back out?
     *
     * True when it holds something outside atext -- a space, a comma, a
     * quote -- so that "a,b"@c.com round trips as itself and not as the
     * unquoted nonsense a naive concatenation would produce.
     */
    bool must_quote() const;

    /** Canonical: local@domain, requoting and rebracketing as needed. */
    std::string str() const;

protected:
    std::string m_local;
    std::string m_domain;
    bool m_literal = false;

    friend struct reader;
    friend class mailbox;
};

/**
 * An address with everything around it: a display name, a group, the comments.
 */
class mailbox {
public:
    typedef address::exception exception;

    mailbox() = default;

    /** The whole of s must be one mailbox. */
    static mailbox parse(std::string_view s);
    static mailbox parse(std::string_view s, const policy& p);

    /**
     * A header field: "a@b, Joe <c@d>, Group: e@f, g@h;".
     *
     * RFC 5322's address-list, which is why it flattens groups -- a group's
     * members are mailboxes and a caller sending mail wants all of them.  Each
     * one remembers the group it came from.
     *
     * Under the default policy this takes obs-addr-list, so the leading,
     * doubled and trailing commas that real headers are littered with are
     * accepted and produce no empty entries.
     */
    static std::vector<mailbox> parse_list(std::string_view s);
    static std::vector<mailbox> parse_list(std::string_view s, const policy& p);

    /** The display name, unquoted, or "" when there was none. */
    const std::string& name() const { return m_name; }

    const address& addr() const { return m_addr; }

    /** The group this came from, or "" when it stood on its own. */
    const std::string& group() const { return m_group; }

    /**
     * What was written, verbatim but for the whitespace around it.
     *
     * Comments included: "joe@x.com (Joe Bloggs)" comes back whole, because
     * that is what the user typed and a recipient list should show it.
     */
    const std::string& source() const { return m_source; }

    /** Canonical: "Display Name <local@domain>", or the bare address. */
    std::string str() const;

protected:
    std::string m_name;
    address m_addr;
    std::string m_group;
    std::string m_source;

    friend struct reader;
};

}
}

#endif // JLIB_NET_ADDRESS_HH
