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

#ifndef JLIB_UTIL_XML_HH
#define JLIB_UTIL_XML_HH

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jlib {
namespace util {

/**
 * A small XML reader and document tree.
 *
 * The grammar is written in jlib::util::abnf rather than scanned by hand,
 * which is most of why this is short.  End tags are matched with abnf's
 * backref(), so a mismatched close is a parse error from the grammar rather
 * than a check bolted on afterwards.
 *
 * ## What it handles
 *
 * Elements, attributes, character data, comments, the processing instruction,
 * self-closing tags, and the three predefined entities &amp; &lt; &gt; through
 * the codec that already lives in util.hh.
 *
 * ## What it does not
 *
 * No namespaces, no DTD or DOCTYPE, no CDATA sections, no numeric character
 * references, no encoding declarations -- the declared encoding is parsed as
 * an ordinary attribute and then ignored, so input is whatever bytes it was.
 * These are not oversights to be fixed quietly later; a document using any of
 * them is rejected with a position rather than half-read, and the test says so
 * as plainly as this does.
 *
 * ## Whitespace
 *
 * Kept.  Text between elements becomes a text node like any other, so a
 * document written out again is byte-identical to the one read in.  Nothing
 * here pretty-prints, because it cannot do that and preserve the input.
 */
namespace xml {

/** A document that could not be read, and where it stopped making sense. */
class error : public std::runtime_error {
public:
    error(const std::string& msg, std::size_t line, std::size_t column);

    std::size_t line() const { return m_line; }
    std::size_t column() const { return m_column; }

protected:
    std::size_t m_line;
    std::size_t m_column;
};

/** What a node is.  There are only three kinds. */
enum class kind { element, text, instruction };

class node;

typedef std::shared_ptr<node> ptr;
typedef std::shared_ptr<const node> const_ptr;

/**
 * An element, a run of text, or the leading <?...?>.
 *
 * Built through the static makers rather than a constructor, so a node is
 * always held by shared_ptr and a tree can be passed around without deciding
 * who owns it.
 */
class node {
public:
    typedef std::vector<ptr> child_list;
    typedef std::pair<std::string, std::string> attribute;

    /**
     * Attributes in document order, duplicates and all.
     *
     * A vector rather than a map on purpose.  A map loses the order they were
     * written in, which makes writing a document back out reorder it, and it
     * silently drops a repeated attribute instead of letting a caller notice
     * that the document is malformed.  Both are correctness properties, and
     * neither is recoverable once the storage has thrown them away.
     *
     * The cost is that get() and has() are a linear scan.  Measured, in
     * nanoseconds per lookup averaged over every key:
     *
     *     attributes      2     8    32    64   256
     *     this            14    26    47    88   164
     *     std::map        17    24    35    47    67
     *     unordered_map   15    14    19    20    22
     *
     * So the crossover is around eight, and past thirty or so this is
     * meaningfully worse.  It is kept anyway because real elements do not get
     * there -- gtkmail's <address> carries three attributes and its mailbox
     * <config> around twenty, which is the largest in either program -- and
     * because an element with hundreds of attributes is not a shape XML
     * produces.
     *
     * The obvious answer -- keep the vector for order and add a hash from name
     * to index for lookup -- was measured before being believed, and it loses.
     * Twenty thousand elements, building the document and then doing one
     * lookup per element for the *last* attribute, which is the scan's worst
     * case:
     *
     *     attributes        3            8           20
     *     build   plain     1.97 ms      3.41 ms      9.51 ms
     *             indexed   3.60         7.33        19.41
     *     lookup  plain     0.199        0.518        2.166
     *             indexed   0.247        0.727        1.383
     *
     * At three and eight attributes the index is slower at *both* ends: a hash
     * over that few short strings costs more to build and more to probe than
     * scanning them costs.  At twenty it finally wins the lookup, by 0.78ms --
     * against 9.9ms of extra construction, so it needs about thirteen lookups
     * of the same element before it has paid for itself.
     *
     * And the access pattern is one lookup per element.  A caller searching an
     * address book does get("key") on each in turn, so the cost scales with
     * the number of elements and an index per element is bought and thrown
     * away.  If that search is ever slow the answer is an index over the
     * addresses, in the program that owns them, not over attributes here.
     */
    typedef std::vector<attribute> attribute_list;

    static ptr element(std::string name);
    static ptr text(std::string content);
    static ptr instruction(std::string target, std::string body);

    kind type() const { return m_kind; }

    /** The tag name, or the target of a processing instruction. */
    const std::string& name() const { return m_name; }

    /** The character data, or the body of a processing instruction. */
    const std::string& content() const { return m_content; }
    void set_content(std::string s) { m_content = std::move(s); }

    bool has(std::string_view name) const;

    /** The attribute's value, or fallback when it is not there. */
    std::string get(std::string_view name,
                    const std::string& fallback = std::string()) const;

    /** Appends; it does not replace an attribute of the same name. */
    void set(std::string name, std::string value);

    const attribute_list& attributes() const { return m_attributes; }

    const child_list& children() const { return m_children; }

    /**
     * Whether an element with no children is written <a/> or <a></a>.
     *
     * Carried so that reading a document and writing it out again gives back
     * the same bytes.  The two spellings mean the same thing, and rewriting
     * one as the other is the sort of gratuitous change that makes a diff
     * unreadable when a config file is edited by hand.
     */
    bool empty_tag() const { return m_empty_tag; }
    void set_empty_tag(bool b) { m_empty_tag = b; }

    /** Appends, and returns what was added so calls can be chained. */
    ptr add(ptr child);

    /** The first child element with that name, or null. */
    ptr first(std::string_view name) const;

    /** Every child element with that name, in document order. */
    child_list all(std::string_view name) const;

    /** Every text descendant, concatenated. */
    std::string text_content() const;

    /** Write this subtree.  Adds no whitespace of its own. */
    void write(std::ostream& os) const;

    std::string str() const;

protected:
    node(kind k, std::string name, std::string content);

    kind m_kind;
    std::string m_name;
    std::string m_content;
    attribute_list m_attributes;
    child_list m_children;
    bool m_empty_tag = true;
};

/**
 * Read a document, and return its root element.
 *
 * The processing instruction, if there is one, is not the root and is not
 * returned -- nothing in this library has ever needed it, and a caller that
 * does can ask for it here rather than everywhere.
 *
 * Throws xml::error, which carries a line and a column.
 */
ptr parse(std::string_view text);
ptr parse(std::istream& is);

/** Write a document.  Adds no whitespace of its own. */
void write(std::ostream& os, const ptr& root);
std::string to_string(const ptr& root);

}
}
}

#endif // JLIB_UTIL_XML_HH
