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

#ifndef JLIB_UTIL_ABNF_HH
#define JLIB_UTIL_ABNF_HH

#include <bitset>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace jlib {
namespace util {

/**
 * Grammars, and the machinery to run them against input.
 *
 * This is the layer the RFCs are eventually written in.  Two of them, in fact:
 * these combinators, and -- built on top, in a later branch -- a reader for
 * ABNF grammar text, so an RFC's own grammar can be pasted in as it stands.
 * Both are public, because ABNF cannot express everything the RFCs need.
 * RFC 3501 says
 *
 *     literal = "{" number "}" CRLF *CHAR8
 *             ; Number represents the number of CHAR8s
 *
 * with the actual constraint in a *comment*, because there is no way to write
 * it.  counted() is that constraint, and it is why the combinators have to
 * stay reachable rather than being an implementation detail of the text
 * front end.
 *
 * ## What this is not
 *
 * **It is not RFC 5234 semantics.** Alternation here is PEG ordered choice:
 * the first branch that matches wins, and the others are not tried unless it
 * fails.  RFC 5234's `/` is unordered.  The difference is observable, and
 * RFC 5234's own grammar contains an instance of it -- see the note on
 * operator| below.
 *
 * **Repetition is possessive.** `*a` matches as much as it can and never gives
 * any of it back, so `*CHAR "@"` does not parse `a@b`: the repetition eats the
 * `@` and the concatenation then has nothing to match.  until(), counted() and
 * backref() are the ways round it.
 *
 * Both are deliberate, both are tested, and both are stated here rather than
 * discovered.
 */
namespace abnf {

namespace detail {
class expr;
class slot;
class context;
struct arena;
struct capture_record;
}

class match;
class parse_result;
class rule;
struct options;

// ---------------------------------------------------------------- exceptions

/**
 * Base for everything this throws.
 *
 * A departure from the convention of a nested exception per class: the same
 * three failures arise from rule, grammar and match alike, and three nested
 * types for one failure mode would be worse than one hierarchy.  What the
 * convention actually buys -- an identifiable prefixed message, and a single
 * catch that works -- is preserved: every what() begins with the qualified
 * name of the type that threw.
 */
class exception : public std::runtime_error {
public:
    explicit exception(const std::string& msg)
        : std::runtime_error(msg) {}
};

/** The input did not match.  Expected at runtime, and carries a position. */
class error : public exception {
public:
    error(const std::string& msg, std::string input, std::size_t offset,
          std::vector<std::string> expected, std::vector<std::string> stack);

    std::size_t offset() const { return m_offset; }

    /** 1-based, computed on demand -- the success path pays nothing. */
    std::size_t line() const;
    std::size_t column() const;

    /** The line the failure is on, without its terminator. */
    std::string context_line() const;

    /** Terminals and rules that could have continued at offset(). */
    const std::vector<std::string>& expected() const { return m_expected; }

    /** The named rules that were open when the furthest failure happened. */
    const std::vector<std::string>& rule_stack() const { return m_stack; }

    /** Several lines, with a caret under the offending column. */
    std::string report() const;

protected:
    std::string m_input;
    std::size_t m_offset;
    std::vector<std::string> m_expected;
    std::vector<std::string> m_stack;
};

/**
 * The grammar itself is broken.
 *
 * Left recursion, a rulename referenced and never defined, a repetition of
 * something that can match nothing, or -- once the text front end exists --
 * ABNF that does not parse.
 */
class grammar_error : public exception {
public:
    explicit grammar_error(const std::string& msg)
        : exception("jlib::util::abnf::grammar_error: " + msg) {}
};

/**
 * The parse ran too long or too deep.
 *
 * Not a failure of the input so much as a refusal to keep going.  Ordered
 * choice over a grammar with shared prefixes can be exponential, and recursive
 * descent over deeply nested input can exhaust the stack -- and both are
 * reachable from the network, where the input is chosen by somebody else.
 * This turns a hang or a SIGSEGV into something with a position on it.
 */
class budget_exceeded : public error {
public:
    budget_exceeded(const std::string& msg, std::string input,
                    std::size_t offset);
};

// ------------------------------------------------------------------- options

/**
 * What to keep, and what to refuse.
 */
struct options {
    /**
     * Which matches are recorded.
     *
     * none keeps nothing and is the cheapest way to ask "does this parse".
     * named keeps every rule built with as(); for a grammar the size of
     * RFC 5322 that is still hundreds of records per address.  listed keeps
     * only the names asked for, which is usually what a caller wants:
     *
     *     r.parse(text, { .captures = options::capture_policy::listed,
     *                     .capture_only = {"local-part", "domain"} });
     */
    enum class capture_policy { none, named, listed };

    capture_policy captures = capture_policy::named;
    std::set<std::string> capture_only;

    /** 0 means 1000 * input.size() + 100000. */
    std::size_t step_budget = 0;

    /** Nested rule invocations before budget_exceeded. */
    std::size_t max_depth = 1000;
};

// --------------------------------------------------------------------- match

/**
 * A matched span, and a cursor over the ones inside it.
 *
 * Sixteen bytes: everything is held in one arena that the whole result shares,
 * and this is an index into it.  Copying a match is free.
 *
 * The arena owns a copy of the input, so a match cannot outlive its text the
 * way a string_view into the caller's buffer would.
 */
class match {
public:
    typedef std::vector<match> list;
    typedef list::value_type value_type;
    typedef list::size_type size_type;
    typedef list::const_iterator const_iterator;

    match();

    /** "" for the root, which is anonymous. */
    std::string name() const;

    std::string_view text() const;
    std::string str() const { return std::string(text()); }

    std::size_t begin() const;
    std::size_t end() const;

    bool has(std::string_view name) const;

    /**
     * The first descendant with that name, at any depth.
     *
     * Descendant rather than child on purpose: a caller asking for
     * "local-part" should not have to know it sits under addr-spec under
     * mailbox.  Where a name occurs at more than one depth this returns the
     * shallowest, then the earliest; child() is the strict version.
     */
    match operator[](std::string_view name) const;
    match child(std::string_view name) const;

    /** Every descendant with that name, in document order. */
    list all(std::string_view name) const;
    list children() const;

    /** False for the match returned when a name was not found. */
    explicit operator bool() const;

protected:
    friend class rule;
    match(std::shared_ptr<const detail::arena> a, std::size_t i);

    std::shared_ptr<const detail::arena> m_arena;
    std::size_t m_index;
};

/**
 * The outcome of a parse that was allowed to fail.
 *
 * The library throws rather than returns, as the rest of jlib does, so
 * rule::parse() is the documented path.  This exists for the caller in a loop
 * over input that is expected to be malformed -- extracting addresses from a
 * mailbox, say -- where a throw per line is the wrong shape.
 */
class parse_result {
public:
    parse_result();

    explicit operator bool() const { return m_ok; }

    /** Throws if the parse failed. */
    match root() const;

    /** Valid only when the parse failed. */
    const error& why() const;

    /** Bytes consumed.  Less than the input for a deliberate partial parse. */
    std::size_t consumed() const { return m_consumed; }

protected:
    friend class rule;

    bool m_ok;
    std::size_t m_consumed;
    match m_root;
    std::shared_ptr<error> m_why;
};

// ---------------------------------------------------------------------- rule

/**
 * A grammar rule: a value handle over an immutable expression.
 *
 * Copying is a reference count; sharing one rule between several grammars is
 * free and is the normal case.  Rules are immutable once built, with one
 * exception -- a rule from declare() owns a cell that define() fills in later,
 * which is how forward references and recursion work.
 */
class rule {
public:
    typedef std::shared_ptr<const detail::expr> ptr;

    /** For the max of rep(): as many as there are. */
    static const std::size_t unbounded = static_cast<std::size_t>(-1);

    /** Matches the empty string. */
    rule();
    explicit rule(ptr e);

    /**
     * A named rule with no body yet.
     *
     * RFC grammars are not written in dependency order, so a rule routinely
     * mentions one defined further down.  This is also how a rule refers to
     * itself.  Parsing one that was never defined is a grammar_error.
     */
    static rule declare(std::string name);

    /** Fill in a declared rule.  Throws if it was not declared, or is done. */
    void define(const rule& body);

    /**
     * Add an alternative to a rule already defined -- ABNF's "=/".
     *
     * Every reference to the rule sees this, because they all point at the
     * cell rather than at the body.
     */
    void define_alternative(const rule& more);

    bool declared() const;
    bool defined() const;

    /** "" when the rule is anonymous. */
    std::string name() const;

    /** Parse the whole input.  Throws error if it does not match. */
    match parse(std::string_view in) const;
    match parse(std::string_view in, const options& o) const;

    /** As parse(), but a mismatch is a return value rather than a throw. */
    parse_result try_parse(std::string_view in) const;
    parse_result try_parse(std::string_view in, const options& o) const;

    /** Canonical ABNF for this rule.  A serialization, not the source text. */
    std::string to_abnf() const;

    ptr node() const { return m_expr; }

protected:
    ptr m_expr;
};

std::ostream& operator<<(std::ostream& os, const rule& r);

// ----------------------------------------------------------------- terminals

/**
 * A literal string, matched case-SENSITIVELY.
 *
 * Note the divergence, because it is deliberate and it is the opposite way
 * round from ABNF.  RFC 5234 2.3 makes a quoted string in a grammar
 * case-insensitive, so the text front end compiles char-val to ilit() -- a
 * pasted grammar must behave as the RFC says.  But in handwritten C++,
 * lit("HELO") quietly also matching "helo" is a trap with nothing to
 * recommend it, so here the plain spelling is the literal one.
 */
rule lit(std::string s);

/** A literal string, matched without regard to ASCII case. */
rule ilit(std::string s);

rule chr(unsigned char c);
rule rng(unsigned char lo, unsigned char hi);

/** An exact sequence of octets, never case-folded.  ABNF's %x0D.0A. */
rule bytes(std::initializer_list<unsigned char> b);

/** Any one of these octets. */
rule anyof(std::string_view chars);

/** Matches nothing at all, and never succeeds. */
rule none();

/** Succeeds without consuming anything. */
rule empty();

// --------------------------------------------------------------- combinators

rule operator>>(const rule& a, const rule& b);

/**
 * Ordered choice: a, and only if that fails, b.
 *
 * C++ precedence happens to be exactly right -- >> is tighter than | -- so
 * `a >> b | c >> d` groups as `(a >> b) | (c >> d)`, which is what RFC 5234
 * 3.10 specifies for concatenation against alternation.  No parentheses
 * needed, and none should be added for fear of it.
 *
 * What is *not* right is the RFC's claim that `/` is unordered.  Here the
 * first match wins, so alternatives that share a prefix must be written
 * longest-first.  RFC 5234's own grammar has an instance:
 *
 *     repeat = 1*DIGIT / (*DIGIT "*" *DIGIT)
 *
 * Against "3*5" the first alternative matches "3", the enclosing rule then
 * fails on "*", and the choice has already committed.  Written the other way
 * round it parses.  See tests/util_abnf_test.cc, which asserts both.
 */
rule operator|(const rule& a, const rule& b);

/** Zero or more, possessively. */
rule operator*(const rule& a);

/** One or more, possessively. */
rule operator+(const rule& a);

/** Zero or one -- ABNF's [a]. */
rule operator-(const rule& a);
rule opt(const rule& a);

rule rep(const rule& a, std::size_t n);
rule rep(const rule& a, std::size_t min, std::size_t max);

rule seq(std::initializer_list<rule> rs);
rule alt(std::initializer_list<rule> rs);

/** Record what this matches, under a name, for match::operator[]. */
rule as(std::string name, const rule& a);

// -------------------------------------------------- context sensitive pieces

/**
 * A run of octets whose length was matched earlier.
 *
 * The IMAP literal, and the reason the combinator layer is public.  source is
 * the rule whose most recent match holds the count; the default adapter reads
 * it as decimal.
 *
 *     rule number  = +core::DIGIT();
 *     rule literal = lit("{") >> number >> lit("}") >> core::CRLF()
 *                  >> counted(number);
 *
 * A count from a branch that was later abandoned is not visible: the record of
 * it is dropped when the branch is.
 */
rule counted(const rule& source);
rule counted(const rule& source, const rule& element);
rule counted(const rule& source, std::function<std::size_t(std::string_view)> adapt);
rule counted(const rule& source, const rule& element,
             std::function<std::size_t(std::string_view)> adapt);

/**
 * The same text some earlier rule matched.
 *
 * A MIME multipart boundary is a back-reference: the delimiter is whatever the
 * Content-Type header said it was.  So is an XML end tag.
 */
rule backref(const rule& source);
rule backref(const rule& source, bool fold);

/** Everything up to, but not including, the next match of terminator. */
rule until(const rule& terminator);

/**
 * An escape hatch: match by running this.
 *
 * Assumed to depend on things the engine cannot see, so a subtree containing
 * one is never memoized.  where_pure() is for a predicate that genuinely
 * depends on nothing but the input at that position.
 */
rule where(std::string description,
           std::function<bool(std::string_view, std::size_t&)> f);
rule where_pure(std::string description,
                std::function<bool(std::string_view, std::size_t&)> f);

// ------------------------------------------------------------------- grammar

/**
 * A set of named rules.
 *
 * Also the owner that breaks their reference cycles: a rule that mentions
 * itself holds a cell that holds a body that holds the cell, which no
 * reference count will ever free.  This clears every body on the way out.
 * Rules declared outside a grammar and made recursive by hand will leak, and
 * that is the reason to keep them in one of these.
 */
class grammar {
public:
    grammar();
    ~grammar();

    grammar(const grammar&) = delete;
    grammar& operator=(const grammar&) = delete;
    grammar(grammar&&) noexcept;
    grammar& operator=(grammar&&) noexcept;

    /** The named rule.  Declares it if it is not there yet. */
    rule operator[](std::string_view name);

    /** The named rule, or throws.  Does not declare. */
    rule at(std::string_view name) const;

    bool has(std::string_view name) const;

    void define(std::string name, const rule& body);
    void define_alternative(std::string name, const rule& more);

    /** Defined rules, in the order they were defined. */
    std::vector<std::string> rules() const;

    /** Referenced and never defined. */
    std::vector<std::string> undefined() const;

    /**
     * Run the checks a grammar has to pass before it can parse anything.
     *
     * Undefined rules, left recursion, and repetition of something that can
     * match the empty string -- which would loop forever.  Called by the first
     * parse; here so a caller can ask without parsing.
     */
    void check() const;

    std::string to_abnf() const;

protected:
    friend class rule;

    class impl;
    std::unique_ptr<impl> m_impl;
};

// ----------------------------------------------------------------- core rules

/**
 * RFC 5234 Appendix B.
 *
 * Functions rather than objects, and the reason is written down in
 * crypt/curve.hh: Commitment::G() and H() were namespace-scope objects, so
 * they were constructed while the shared library loaded -- before main() could
 * call sodium_init() -- and on Linux that allocated until the process was
 * killed, with no output.  These have the same shape, since CRLF() is built
 * from CR() and LF(): at namespace scope their correctness would depend on
 * initialization order, which is guaranteed only within a translation unit.
 * Function-local statics are built on first use and are thread-safe.
 */
namespace core {

const rule& ALPHA();     ///< %x41-5A / %x61-7A
const rule& BIT();       ///< "0" / "1"
const rule& CHAR();      ///< %x01-7F -- note this excludes NUL
const rule& CR();        ///< %x0D
const rule& CRLF();      ///< CR LF
const rule& CTL();       ///< %x00-1F / %x7F
const rule& DIGIT();     ///< %x30-39
const rule& DQUOTE();    ///< %x22
const rule& HEXDIG();    ///< DIGIT / "A" / "B" / "C" / "D" / "E" / "F"
const rule& HTAB();      ///< %x09
const rule& LF();        ///< %x0A
const rule& LWSP();      ///< *(WSP / CRLF WSP) -- see the warning in the source
const rule& OCTET();     ///< %x00-FF -- and this one includes it
const rule& SP();        ///< %x20
const rule& VCHAR();     ///< %x21-7E
const rule& WSP();       ///< SP / HTAB

}

}
}
}

#endif // JLIB_UTIL_ABNF_HH
