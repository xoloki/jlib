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

#include <jlib/util/abnf.hh>

#include <algorithm>
#include <cctype>
#include <ostream>
#include <sstream>

namespace jlib {
namespace util {
namespace abnf {

static const std::size_t NONE = static_cast<std::size_t>(-1);

namespace detail {

// ------------------------------------------------------------------ the tape

struct capture_record {
    std::string name;
    std::size_t begin;
    std::size_t end;
    std::size_t parent;
};

struct arena {
    std::string input;
    std::vector<capture_record> records;
};

/**
 * Where a node that something back-references last matched.
 *
 * depth is the rule nesting the match was made at, and it is what makes this
 * work under recursion.  Without it, a grammar like
 *
 *     elem = "<" name ">" *elem "</" backref(name) ">"
 *
 * cannot parse <a><b/></a>: name is one node shared by every level, so after
 * the inner element closes, the most recent match of it is still "b" and the
 * outer end tag is compared against the wrong thing.  A record made deeper
 * than the position asking for it belongs to a scope that has already closed.
 */
struct tracked_record {
    const expr* source;
    std::size_t begin;
    std::size_t end;
    std::size_t depth;
};

/**
 * One rulename's cell.
 *
 * A reference points here rather than at a body, which is what makes forward
 * declaration, recursion and "=/" all the same mechanism: replacing the body
 * is seen by every reference at once.
 */
class slot {
public:
    std::string name;
    std::shared_ptr<const expr> body;
    bool seeded = false;
};

/**
 * Everything one parse needs, and nothing that outlives it.
 *
 * Deliberately not on the grammar: a frozen grammar is read-only, so the same
 * one can be parsed against from several threads at once.  Anything cached
 * here rather than there keeps that true.
 */
class context {
public:
    std::string_view input;

    const options* opt = 0;
    std::size_t steps = 0;
    std::size_t budget = 0;
    std::size_t depth = 0;
    std::size_t max_depth = 0;

    std::vector<capture_record> captures;
    std::vector<tracked_record> tracked;
    std::size_t parent = NONE;

    /** Furthest position anything reached, and what it wanted there. */
    std::size_t furthest = 0;
    std::vector<std::string> expected;
    std::vector<std::string> stack;
    std::vector<std::string> furthest_stack;

    struct mark {
        std::size_t captures;
        std::size_t tracked;
    };

    mark save() const { return mark{captures.size(), tracked.size()}; }

    void restore(const mark& m)
    {
        captures.resize(m.captures);
        tracked.resize(m.tracked);
    }

    bool keeping(const std::string& name) const
    {
        switch(opt->captures) {
        case options::capture_policy::none:
            return false;
        case options::capture_policy::named:
            return true;
        case options::capture_policy::listed:
            return opt->capture_only.count(name) != 0;
        }

        return false;
    }

    /** Note that something wanted `what` at `pos` and did not get it. */
    void want(std::size_t pos, const std::string& what)
    {
        if(pos > furthest) {
            furthest = pos;
            expected.clear();
            furthest_stack = stack;
        }

        if(pos == furthest &&
           std::find(expected.begin(), expected.end(), what) == expected.end()) {
            expected.push_back(what);
        }
    }
};

// ------------------------------------------------------------------ the node

class expr {
public:
    typedef std::shared_ptr<const expr> ptr;

    expr() = default;
    expr(const expr&) = default;
    expr(expr&&) = default;
    expr& operator=(const expr&) = default;
    expr& operator=(expr&&) = default;
    virtual ~expr() = default;

    virtual bool parse(context& ctx, std::size_t& pos) const = 0;

    /** Serialize, parenthesizing when the context binds tighter than we do. */
    virtual void write(std::ostream& os, int prec) const = 0;

    /** Direct children, for the analysis passes. */
    virtual void kids(std::vector<const expr*>& out) const { (void)out; }

    /** Can this match the empty string?  visiting breaks reference cycles. */
    virtual bool nullable(std::set<const slot*>& visiting) const = 0;

    /** Slots reachable without consuming anything.  For left recursion. */
    virtual void leftmost(std::set<const slot*>& out,
                          std::set<const slot*>& visiting) const = 0;

    /**
     * True unless the subtree's answer can depend on more than the input at
     * a position -- which counted, backref and an impure predicate all make
     * it.  Nothing reads it yet; a memo table would, and a memo keyed on
     * (rule, position) is a lie for exactly those nodes.  Computing it here
     * while the shape is fresh is cheaper than retrofitting it under a
     * memoizer later and discovering IMAP literals were being cached.
     */
    virtual bool pure() const { return true; }

    /** Some counted() or backref() names this node as its source. */
    mutable bool m_tracked = false;
};

/**
 * Every internal call goes through here rather than calling parse() directly.
 *
 * One place to count steps, and one place to record the extent of a node
 * something else back-references.
 */
static bool run(const expr* e, context& ctx, std::size_t& pos);

// ------------------------------------------------------------------ terminals

class charclass : public expr {
public:
    charclass(std::bitset<256> set, std::string label)
        : m_set(set), m_label(std::move(label)) {}

    virtual bool parse(context& ctx, std::size_t& pos) const
    {
        if(pos < ctx.input.size() &&
           m_set.test(static_cast<unsigned char>(ctx.input[pos]))) {
            pos++;
            return true;
        }

        ctx.want(pos, m_label);
        return false;
    }

    virtual void write(std::ostream& os, int) const { os << m_label; }

    virtual bool nullable(std::set<const slot*>&) const { return false; }
    virtual void leftmost(std::set<const slot*>&, std::set<const slot*>&) const {}

protected:
    std::bitset<256> m_set;
    std::string m_label;
};

class literal : public expr {
public:
    literal(std::string s, bool fold)
        : m_text(std::move(s)), m_fold(fold) {}

    virtual bool parse(context& ctx, std::size_t& pos) const
    {
        if(pos + m_text.size() > ctx.input.size()) {
            ctx.want(pos, label());
            return false;
        }

        for(std::size_t i = 0; i < m_text.size(); i++) {
            char a = ctx.input[pos + i];
            char b = m_text[i];

            if(m_fold) {
                a = static_cast<char>(std::tolower(static_cast<unsigned char>(a)));
                b = static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
            }

            if(a != b) {
                ctx.want(pos, label());
                return false;
            }
        }

        pos += m_text.size();
        return true;
    }

    virtual void write(std::ostream& os, int) const { os << label(); }

    virtual bool nullable(std::set<const slot*>&) const { return m_text.empty(); }
    virtual void leftmost(std::set<const slot*>&, std::set<const slot*>&) const {}

protected:
    std::string label() const
    {
        return (m_fold ? "\"" : "%s\"") + m_text + "\"";
    }

    std::string m_text;
    bool m_fold;
};

class nothing : public expr {
public:
    explicit nothing(bool succeed) : m_succeed(succeed) {}

    virtual bool parse(context& ctx, std::size_t& pos) const
    {
        (void)pos;
        if(!m_succeed) {
            ctx.want(pos, "<nothing>");
        }
        return m_succeed;
    }

    virtual void write(std::ostream& os, int) const
    {
        os << (m_succeed ? "\"\"" : "<never>");
    }

    virtual bool nullable(std::set<const slot*>&) const { return m_succeed; }
    virtual void leftmost(std::set<const slot*>&, std::set<const slot*>&) const {}

protected:
    bool m_succeed;
};

// ---------------------------------------------------------------- structural

/** Precedence for write(): alternation loosest, then concatenation. */
enum { PREC_ALT = 1, PREC_CAT = 2, PREC_REP = 3, PREC_ATOM = 4 };

class concat : public expr {
public:
    explicit concat(std::vector<ptr> parts) : m_parts(std::move(parts)) {}

    virtual bool parse(context& ctx, std::size_t& pos) const
    {
        const context::mark m = ctx.save();
        const std::size_t start = pos;

        for(const ptr& p : m_parts) {
            if(!run(p.get(), ctx, pos)) {
                pos = start;
                ctx.restore(m);
                return false;
            }
        }

        return true;
    }

    virtual void write(std::ostream& os, int prec) const
    {
        const bool paren = prec > PREC_CAT;
        if(paren) os << "(";

        for(std::size_t i = 0; i < m_parts.size(); i++) {
            if(i) os << " ";
            m_parts[i]->write(os, PREC_CAT);
        }

        if(paren) os << ")";
    }

    virtual void kids(std::vector<const expr*>& out) const
    {
        for(const ptr& p : m_parts) out.push_back(p.get());
    }

    virtual bool nullable(std::set<const slot*>& v) const
    {
        for(const ptr& p : m_parts)
            if(!p->nullable(v)) return false;

        return true;
    }

    virtual void leftmost(std::set<const slot*>& out,
                          std::set<const slot*>& visiting) const
    {
        // Leftmost reaches past anything that can match nothing.
        for(const ptr& p : m_parts) {
            p->leftmost(out, visiting);

            std::set<const slot*> fresh;
            if(!p->nullable(fresh)) break;
        }
    }

    virtual bool pure() const
    {
        for(const ptr& p : m_parts)
            if(!p->pure()) return false;

        return true;
    }

    const std::vector<ptr>& parts() const { return m_parts; }

protected:
    std::vector<ptr> m_parts;
};

class alternation : public expr {
public:
    explicit alternation(std::vector<ptr> branches)
        : m_branches(std::move(branches)) {}

    virtual bool parse(context& ctx, std::size_t& pos) const
    {
        const context::mark m = ctx.save();
        const std::size_t start = pos;

        for(const ptr& b : m_branches) {
            if(run(b.get(), ctx, pos)) {
                return true;
            }

            // Everything the branch did is undone before the next is tried.
            // This is what "full backtracking" means here, and it is what lets
            // mailbox = name-addr / addr-spec run a display name across a bare
            // address, fail at the @, and rescan from the beginning.
            pos = start;
            ctx.restore(m);
        }

        return false;
    }

    virtual void write(std::ostream& os, int prec) const
    {
        const bool paren = prec > PREC_ALT;
        if(paren) os << "(";

        for(std::size_t i = 0; i < m_branches.size(); i++) {
            if(i) os << " / ";
            m_branches[i]->write(os, PREC_ALT);
        }

        if(paren) os << ")";
    }

    virtual void kids(std::vector<const expr*>& out) const
    {
        for(const ptr& b : m_branches) out.push_back(b.get());
    }

    virtual bool nullable(std::set<const slot*>& v) const
    {
        for(const ptr& b : m_branches)
            if(b->nullable(v)) return true;

        return false;
    }

    virtual void leftmost(std::set<const slot*>& out,
                          std::set<const slot*>& visiting) const
    {
        for(const ptr& b : m_branches) b->leftmost(out, visiting);
    }

    virtual bool pure() const
    {
        for(const ptr& b : m_branches)
            if(!b->pure()) return false;

        return true;
    }

    const std::vector<ptr>& branches() const { return m_branches; }

protected:
    std::vector<ptr> m_branches;
};

class repeat : public expr {
public:
    repeat(ptr body, std::size_t min, std::size_t max)
        : m_body(std::move(body)), m_min(min), m_max(max) {}

    virtual bool parse(context& ctx, std::size_t& pos) const
    {
        const context::mark m = ctx.save();
        const std::size_t start = pos;

        std::size_t n = 0;

        while(n < m_max) {
            std::size_t at = pos;

            if(!run(m_body.get(), ctx, at)) {
                break;
            }

            // A body that matched without consuming would spin forever.  The
            // grammar check rejects that shape up front for an unbounded
            // repetition; this is the belt to its braces, and it also covers
            // a bounded one, where the check lets it through.
            if(at == pos) {
                break;
            }

            pos = at;
            n++;
        }

        if(n < m_min) {
            pos = start;
            ctx.restore(m);
            return false;
        }

        return true;
    }

    virtual void write(std::ostream& os, int prec) const
    {
        const bool paren = prec > PREC_REP;
        if(paren) os << "(";

        if(m_min == 0 && m_max == 1) {
            os << "[";
            m_body->write(os, PREC_ALT);
            os << "]";

            if(paren) os << ")";
            return;
        }

        if(m_min != 0) os << m_min;
        os << "*";
        if(m_max != rule::unbounded) os << m_max;

        m_body->write(os, PREC_REP + 1);

        if(paren) os << ")";
    }

    virtual void kids(std::vector<const expr*>& out) const
    {
        out.push_back(m_body.get());
    }

    virtual bool nullable(std::set<const slot*>& v) const
    {
        return m_min == 0 || m_body->nullable(v);
    }

    virtual void leftmost(std::set<const slot*>& out,
                          std::set<const slot*>& visiting) const
    {
        m_body->leftmost(out, visiting);
    }

    virtual bool pure() const { return m_body->pure(); }

    const ptr& body() const { return m_body; }
    std::size_t min() const { return m_min; }
    std::size_t max() const { return m_max; }

protected:
    ptr m_body;
    std::size_t m_min;
    std::size_t m_max;
};

// ------------------------------------------------------------------ reference

class reference : public expr {
public:
    explicit reference(std::shared_ptr<slot> s) : m_slot(std::move(s)) {}

    virtual bool parse(context& ctx, std::size_t& pos) const
    {
        if(!m_slot->body) {
            throw grammar_error("rule \"" + m_slot->name +
                                "\" is referenced but never defined");
        }

        if(ctx.depth >= ctx.max_depth) {
            throw budget_exceeded(
                "recursion passed " + std::to_string(ctx.max_depth) +
                " while parsing \"" + m_slot->name + "\"",
                std::string(ctx.input), pos);
        }

        ctx.depth++;
        ctx.stack.push_back(m_slot->name);

        const bool ok = run(m_slot->body.get(), ctx, pos);

        ctx.stack.pop_back();
        ctx.depth--;

        if(!ok) {
            ctx.want(pos, m_slot->name);
        }

        return ok;
    }

    virtual void write(std::ostream& os, int) const { os << m_slot->name; }

    virtual bool nullable(std::set<const slot*>& v) const
    {
        if(!m_slot->body || v.count(m_slot.get())) {
            return false;
        }

        v.insert(m_slot.get());
        const bool n = m_slot->body->nullable(v);
        v.erase(m_slot.get());

        return n;
    }

    virtual void leftmost(std::set<const slot*>& out,
                          std::set<const slot*>& visiting) const
    {
        out.insert(m_slot.get());

        if(!m_slot->body || visiting.count(m_slot.get())) {
            return;
        }

        visiting.insert(m_slot.get());
        m_slot->body->leftmost(out, visiting);
        visiting.erase(m_slot.get());
    }

    virtual bool pure() const
    {
        // Not followed through the slot: the body may not be defined yet, and
        // a cycle would not terminate.  Assuming a reference impure would
        // disable memoization everywhere; assuming it pure is safe only
        // because nothing memoizes yet, and the check moves into freeze()
        // when something does.
        return true;
    }

    const std::shared_ptr<slot>& cell() const { return m_slot; }

protected:
    std::shared_ptr<slot> m_slot;
};

// -------------------------------------------------------------------- capture

class capture : public expr {
public:
    capture(std::string name, ptr body)
        : m_name(std::move(name)), m_body(std::move(body)) {}

    virtual bool parse(context& ctx, std::size_t& pos) const
    {
        const bool keep = ctx.keeping(m_name);

        std::size_t index = NONE;
        const std::size_t start = pos;
        const std::size_t was = ctx.parent;

        if(keep) {
            index = ctx.captures.size();
            ctx.captures.push_back(capture_record{m_name, start, start, was});
            ctx.parent = index;
        }

        const bool ok = run(m_body.get(), ctx, pos);

        ctx.parent = was;

        if(keep) {
            if(ok) {
                ctx.captures[index].end = pos;
            }
            else {
                ctx.captures.resize(index);
            }
        }

        return ok;
    }

    virtual void write(std::ostream& os, int prec) const
    {
        m_body->write(os, prec);
    }

    virtual void kids(std::vector<const expr*>& out) const
    {
        out.push_back(m_body.get());
    }

    virtual bool nullable(std::set<const slot*>& v) const
    {
        return m_body->nullable(v);
    }

    virtual void leftmost(std::set<const slot*>& out,
                          std::set<const slot*>& visiting) const
    {
        m_body->leftmost(out, visiting);
    }

    virtual bool pure() const { return m_body->pure(); }

    const std::string& name() const { return m_name; }

protected:
    std::string m_name;
    ptr m_body;
};

// ------------------------------------------------------- context sensitivity

/**
 * The most recent extent of source that is still in scope.
 *
 * Backtracking has already removed anything from a branch that lost; this
 * additionally skips matches made inside a nesting level that has since
 * closed.  See tracked_record.
 */
static const tracked_record* last(const context& ctx, const expr* source)
{
    for(std::size_t i = ctx.tracked.size(); i > 0; i--) {
        if(ctx.tracked[i-1].source == source &&
           ctx.tracked[i-1].depth <= ctx.depth) {
            return &ctx.tracked[i-1];
        }
    }

    return 0;
}

class counted_node : public expr {
public:
    counted_node(ptr source, ptr element,
                 std::function<std::size_t(std::string_view)> adapt)
        : m_source(std::move(source)),
          m_element(std::move(element)),
          m_adapt(std::move(adapt))
    {
        m_source->m_tracked = true;
    }

    virtual bool parse(context& ctx, std::size_t& pos) const
    {
        const tracked_record* r = last(ctx, m_source.get());

        if(!r) {
            throw error("jlib::util::abnf::error: counted() has no prior match "
                        "of its source rule",
                        std::string(ctx.input), pos, {}, ctx.stack);
        }

        const std::size_t n =
            m_adapt(ctx.input.substr(r->begin, r->end - r->begin));

        if(!m_element) {
            if(pos + n > ctx.input.size()) {
                ctx.want(pos, std::to_string(n) + " octets");
                return false;
            }

            pos += n;
            return true;
        }

        const context::mark m = ctx.save();
        const std::size_t start = pos;

        for(std::size_t i = 0; i < n; i++) {
            if(!run(m_element.get(), ctx, pos)) {
                pos = start;
                ctx.restore(m);
                return false;
            }
        }

        return true;
    }

    virtual void write(std::ostream& os, int) const
    {
        os << "<counted>";
    }

    virtual bool nullable(std::set<const slot*>&) const
    {
        // Depends on a runtime count, so it might match nothing.  Saying yes
        // is the safe answer: it makes *counted(...) a grammar error rather
        // than a possible infinite loop.
        return true;
    }

    virtual void leftmost(std::set<const slot*>&, std::set<const slot*>&) const {}
    virtual bool pure() const { return false; }

protected:
    ptr m_source;
    ptr m_element;
    std::function<std::size_t(std::string_view)> m_adapt;
};

class backref_node : public expr {
public:
    backref_node(ptr source, bool fold)
        : m_source(std::move(source)), m_fold(fold)
    {
        m_source->m_tracked = true;
    }

    virtual bool parse(context& ctx, std::size_t& pos) const
    {
        const tracked_record* r = last(ctx, m_source.get());

        if(!r) {
            throw error("jlib::util::abnf::error: backref() has no prior match "
                        "of its source rule",
                        std::string(ctx.input), pos, {}, ctx.stack);
        }

        const std::string_view want = ctx.input.substr(r->begin, r->end - r->begin);

        if(pos + want.size() > ctx.input.size()) {
            ctx.want(pos, "the earlier \"" + std::string(want) + "\"");
            return false;
        }

        for(std::size_t i = 0; i < want.size(); i++) {
            char a = ctx.input[pos + i];
            char b = want[i];

            if(m_fold) {
                a = static_cast<char>(std::tolower(static_cast<unsigned char>(a)));
                b = static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
            }

            if(a != b) {
                ctx.want(pos, "the earlier \"" + std::string(want) + "\"");
                return false;
            }
        }

        pos += want.size();
        return true;
    }

    virtual void write(std::ostream& os, int) const { os << "<backref>"; }

    virtual bool nullable(std::set<const slot*>&) const { return true; }
    virtual void leftmost(std::set<const slot*>&, std::set<const slot*>&) const {}
    virtual bool pure() const { return false; }

protected:
    ptr m_source;
    bool m_fold;
};

class until_node : public expr {
public:
    explicit until_node(ptr terminator) : m_term(std::move(terminator)) {}

    virtual bool parse(context& ctx, std::size_t& pos) const
    {
        while(pos <= ctx.input.size()) {
            std::size_t at = pos;
            const context::mark m = ctx.save();

            if(run(m_term.get(), ctx, at)) {
                ctx.restore(m);
                return true;
            }

            ctx.restore(m);

            if(pos == ctx.input.size()) {
                break;
            }

            pos++;
        }

        ctx.want(pos, "something the terminator follows");
        return false;
    }

    virtual void write(std::ostream& os, int) const { os << "<until>"; }

    virtual bool nullable(std::set<const slot*>&) const { return true; }
    virtual void leftmost(std::set<const slot*>&, std::set<const slot*>&) const {}
    virtual bool pure() const { return m_term->pure(); }

protected:
    ptr m_term;
};

class predicate : public expr {
public:
    predicate(std::string desc,
              std::function<bool(std::string_view, std::size_t&)> f, bool pure)
        : m_desc(std::move(desc)), m_fn(std::move(f)), m_pure(pure) {}

    virtual bool parse(context& ctx, std::size_t& pos) const
    {
        if(m_fn(ctx.input, pos)) {
            return true;
        }

        ctx.want(pos, m_desc);
        return false;
    }

    virtual void write(std::ostream& os, int) const { os << "<" << m_desc << ">"; }

    /**
     * Assumed to consume something.
     *
     * A predicate is opaque, so this is a guess either way.  It guessed the
     * other way at first, on the theory that the safe answer for something
     * unknown is "it might match nothing" -- and that turned out to block the
     * ordinary use.  A rule like *NameChar, where the predicate decodes one
     * codepoint, is an unbounded repetition of a predicate and was rejected by
     * the grammar check as a guaranteed infinite loop.
     *
     * Guessing this way round is also the cheaper mistake.  If a predicate
     * really can match nothing, repeat::parse stops as soon as an iteration
     * consumes nothing, so the failure is a repetition that ends early rather
     * than one that never ends.
     */
    virtual bool nullable(std::set<const slot*>&) const { return false; }
    virtual void leftmost(std::set<const slot*>&, std::set<const slot*>&) const {}
    virtual bool pure() const { return m_pure; }

protected:
    std::string m_desc;
    std::function<bool(std::string_view, std::size_t&)> m_fn;
    bool m_pure;
};

// ---------------------------------------------------------------------- run

static bool run(const expr* e, context& ctx, std::size_t& pos)
{
    ctx.steps++;

    if(ctx.steps > ctx.budget) {
        throw budget_exceeded(
            "gave up after " + std::to_string(ctx.steps) + " steps",
            std::string(ctx.input), ctx.furthest);
    }

    const std::size_t start = pos;
    const bool ok = e->parse(ctx, pos);

    if(ok && e->m_tracked) {
        ctx.tracked.push_back(tracked_record{e, start, pos, ctx.depth});
    }

    return ok;
}


}  // namespace detail

using detail::expr;
using detail::slot;
using detail::context;
using detail::arena;
using detail::capture_record;

// ------------------------------------------------------------------- errors

error::error(const std::string& msg, std::string input, std::size_t offset,
             std::vector<std::string> expected, std::vector<std::string> stack)
    : exception(msg),
      m_input(std::move(input)),
      m_offset(offset),
      m_expected(std::move(expected)),
      m_stack(std::move(stack))
{
}

std::size_t error::line() const
{
    std::size_t n = 1;

    for(std::size_t i = 0; i < m_offset && i < m_input.size(); i++) {
        if(m_input[i] == '\n') n++;
    }

    return n;
}

std::size_t error::column() const
{
    std::size_t start = 0;

    for(std::size_t i = 0; i < m_offset && i < m_input.size(); i++) {
        if(m_input[i] == '\n') start = i + 1;
    }

    return m_offset - start + 1;
}

std::string error::context_line() const
{
    std::size_t start = 0;

    for(std::size_t i = 0; i < m_offset && i < m_input.size(); i++) {
        if(m_input[i] == '\n') start = i + 1;
    }

    std::size_t stop = m_input.find('\n', start);
    if(stop == std::string::npos) stop = m_input.size();

    return m_input.substr(start, stop - start);
}

std::string error::report() const
{
    std::ostringstream o;

    o << what();

    if(!m_expected.empty()) {
        o << ": expected ";

        for(std::size_t i = 0; i < m_expected.size(); i++) {
            if(i) o << (i + 1 == m_expected.size() ? " or " : ", ");
            o << m_expected[i];
        }
    }

    o << " at line " << line() << ", column " << column() << "\n";
    o << "  " << context_line() << "\n";
    o << "  " << std::string(column() - 1, ' ') << "^";

    if(!m_stack.empty()) {
        o << "\n  while parsing:";

        for(std::size_t i = 0; i < m_stack.size(); i++) {
            o << (i ? " -> " : " ") << m_stack[i];
        }
    }

    return o.str();
}

void error::raise() const { throw *this; }

budget_exceeded::budget_exceeded(const std::string& msg, std::string input,
                                 std::size_t offset)
    : error("jlib::util::abnf::budget_exceeded: " + msg, std::move(input),
            offset, {}, {})
{
}

void budget_exceeded::raise() const { throw *this; }

// -------------------------------------------------------------------- match

match::match() : m_index(NONE) {}

match::match(std::shared_ptr<const arena> a, std::size_t i)
    : m_arena(std::move(a)), m_index(i) {}

match::operator bool() const { return m_arena && m_index != NONE; }

std::string match::name() const
{
    if(!*this || m_index >= m_arena->records.size()) return std::string();

    return m_arena->records[m_index].name;
}

std::size_t match::begin() const
{
    if(!m_arena) return 0;
    if(m_index >= m_arena->records.size()) return 0;

    return m_arena->records[m_index].begin;
}

std::size_t match::end() const
{
    if(!m_arena) return 0;
    if(m_index >= m_arena->records.size()) return m_arena->input.size();

    return m_arena->records[m_index].end;
}

std::string_view match::text() const
{
    if(!m_arena) return std::string_view();

    const std::size_t b = begin(), e = end();

    return std::string_view(m_arena->input).substr(b, e - b);
}

/** Depth-first over the records, which are already in document order. */
static bool descends(const std::vector<capture_record>& rs,
                     std::size_t child, std::size_t ancestor)
{
    if(ancestor == NONE) return true;

    for(std::size_t at = rs[child].parent; at != NONE; at = rs[at].parent) {
        if(at == ancestor) return true;
    }

    return false;
}

match match::operator[](std::string_view name) const
{
    if(!m_arena) return match();

    for(std::size_t i = 0; i < m_arena->records.size(); i++) {
        if(m_arena->records[i].name == name &&
           descends(m_arena->records, i, m_index)) {
            return match(m_arena, i);
        }
    }

    return match();
}

match match::child(std::string_view name) const
{
    if(!m_arena) return match();

    for(std::size_t i = 0; i < m_arena->records.size(); i++) {
        if(m_arena->records[i].name == name &&
           m_arena->records[i].parent == m_index) {
            return match(m_arena, i);
        }
    }

    return match();
}

bool match::has(std::string_view name) const
{
    return static_cast<bool>((*this)[name]);
}

match::list match::all(std::string_view name) const
{
    list out;

    if(!m_arena) return out;

    for(std::size_t i = 0; i < m_arena->records.size(); i++) {
        if(m_arena->records[i].name == name &&
           descends(m_arena->records, i, m_index)) {
            out.push_back(match(m_arena, i));
        }
    }

    return out;
}

match::list match::children() const
{
    list out;

    if(!m_arena) return out;

    for(std::size_t i = 0; i < m_arena->records.size(); i++) {
        if(m_arena->records[i].parent == m_index) {
            out.push_back(match(m_arena, i));
        }
    }

    return out;
}

// -------------------------------------------------------------- parse_result

parse_result::parse_result() : m_ok(false), m_consumed(0) {}

match parse_result::root() const
{
    if(!m_ok) throw *m_why;

    return m_root;
}

const error& parse_result::why() const
{
    if(m_ok) {
        throw exception("jlib::util::abnf::exception: why() on a parse that "
                        "succeeded");
    }

    return *m_why;
}

// --------------------------------------------------------------------- rule

static expr::ptr node_of(const rule& r) { return r.node(); }

rule::rule() : m_expr(std::make_shared<detail::nothing>(true)) {}
rule::rule(ptr e) : m_expr(std::move(e)) {}

rule rule::declare(std::string name)
{
    std::shared_ptr<slot> s = std::make_shared<slot>();
    s->name = std::move(name);

    return rule(std::make_shared<detail::reference>(s));
}

static const detail::reference* as_reference(const expr::ptr& e)
{
    return dynamic_cast<const detail::reference*>(e.get());
}

bool rule::declared() const { return as_reference(m_expr) != 0; }

bool rule::defined() const
{
    const detail::reference* r = as_reference(m_expr);

    return r && r->cell()->body;
}

std::string rule::name() const
{
    const detail::reference* r = as_reference(m_expr);

    return r ? r->cell()->name : std::string();
}

void rule::define(const rule& body)
{
    const detail::reference* r = as_reference(m_expr);

    if(!r) {
        throw grammar_error("define() on a rule that was not declared");
    }

    if(r->cell()->body && !r->cell()->seeded) {
        throw grammar_error("rule \"" + r->cell()->name +
                            "\" is already defined; use define_alternative()"
                            " for ABNF's \"=/\"");
    }

    r->cell()->seeded = false;
    r->cell()->body = body.node();
}

void rule::define_alternative(const rule& more)
{
    const detail::reference* r = as_reference(m_expr);

    if(!r) {
        throw grammar_error("define_alternative() on a rule that was not "
                            "declared");
    }

    if(!r->cell()->body) {
        throw grammar_error("rule \"" + r->cell()->name + "\" has no definition"
                            " to add an alternative to");
    }

    std::vector<expr::ptr> branches;

    // Flatten, so a rule extended five times is one alternation and not five.
    const detail::alternation* a =
        dynamic_cast<const detail::alternation*>(r->cell()->body.get());

    if(a) {
        branches = a->branches();
    }
    else {
        branches.push_back(r->cell()->body);
    }

    branches.push_back(more.node());

    r->cell()->body = std::make_shared<detail::alternation>(std::move(branches));
}

std::string rule::to_abnf() const
{
    std::ostringstream o;
    m_expr->write(o, detail::PREC_ALT);

    return o.str();
}

std::ostream& operator<<(std::ostream& os, const rule& r)
{
    return os << r.to_abnf();
}

/**
 * The first line of report(), for what().
 *
 * Built here rather than left to report() because an error that escapes -- to
 * a terminate handler, or to a caller that wraps it in one of its own -- is
 * seen through what() and nothing else, and "jlib::util::abnf::error" on its
 * own says only which library was unhappy.
 */
static std::string summarise(std::string_view in, std::size_t offset,
                             const std::vector<std::string>& expected)
{
    std::ostringstream o;

    o << "jlib::util::abnf::error";

    if(!expected.empty()) {
        o << ": expected ";

        for(std::size_t i = 0; i < expected.size(); i++) {
            if(i) o << (i + 1 == expected.size() ? " or " : ", ");
            o << expected[i];
        }
    }

    std::size_t line = 1, start = 0;

    for(std::size_t i = 0; i < offset && i < in.size(); i++) {
        if(in[i] == '\n') { line++; start = i + 1; }
    }

    o << " at line " << line << ", column " << (offset - start + 1);

    return o.str();
}

parse_result rule::try_parse(std::string_view in) const
{
    return try_parse(in, options());
}

parse_result rule::try_parse(std::string_view in, const options& o) const
{
    parse_result out;

    context ctx;
    ctx.input = in;
    ctx.opt = &o;
    ctx.budget = o.step_budget ? o.step_budget : (1000 * in.size() + 100000);
    ctx.max_depth = o.max_depth;

    std::size_t pos = 0;
    bool ok = false;

    try {
        ok = detail::run(m_expr.get(), ctx, pos);
    }
    catch(budget_exceeded& e) {
        out.m_why = std::make_shared<budget_exceeded>(e);
        return out;
    }

    if(ok && pos != in.size()) {
        ctx.want(pos, "end of input");
        ok = false;
    }

    out.m_consumed = pos;

    if(!ok) {
        out.m_why = std::make_shared<error>(
            summarise(in, ctx.furthest, ctx.expected), std::string(in),
            ctx.furthest, ctx.expected, ctx.furthest_stack);

        return out;
    }

    std::shared_ptr<arena> a = std::make_shared<arena>();
    a->input = std::string(in);
    a->records = std::move(ctx.captures);

    out.m_ok = true;
    out.m_root = match(a, NONE);

    return out;
}

match rule::parse(std::string_view in) const
{
    return parse(in, options());
}

match rule::parse(std::string_view in, const options& o) const
{
    parse_result r = try_parse(in, o);

    if(!r) r.why().raise();

    return r.root();
}

// ----------------------------------------------------------------- terminals

static std::bitset<256> one(unsigned char c)
{
    std::bitset<256> b;
    b.set(c);

    return b;
}

static std::string hex(unsigned char c)
{
    static const char* digits = "0123456789ABCDEF";

    std::string s = "%x";
    s += digits[(c >> 4) & 0xf];
    s += digits[c & 0xf];

    return s;
}

rule lit(std::string s)
{
    if(s.empty()) return empty();

    return rule(std::make_shared<detail::literal>(std::move(s), false));
}

rule ilit(std::string s)
{
    if(s.empty()) return empty();

    return rule(std::make_shared<detail::literal>(std::move(s), true));
}

rule chr(unsigned char c)
{
    return rule(std::make_shared<detail::charclass>(one(c), hex(c)));
}

rule rng(unsigned char lo, unsigned char hi)
{
    if(lo > hi) {
        throw grammar_error("rng(): " + hex(lo) + " is above " + hex(hi));
    }

    std::bitset<256> b;
    for(unsigned int c = lo; c <= hi; c++) b.set(c);

    return rule(std::make_shared<detail::charclass>(
                    b, hex(lo) + "-" + hex(hi).substr(2)));
}

rule bytes(std::initializer_list<unsigned char> b)
{
    std::string s;
    for(unsigned char c : b) s += static_cast<char>(c);

    return rule(std::make_shared<detail::literal>(std::move(s), false));
}

rule anyof(std::string_view chars)
{
    std::bitset<256> b;
    std::string label = "<any of \"";

    for(char c : chars) {
        b.set(static_cast<unsigned char>(c));
        label += c;
    }

    label += "\">";

    return rule(std::make_shared<detail::charclass>(b, label));
}

rule none() { return rule(std::make_shared<detail::nothing>(false)); }
rule empty() { return rule(std::make_shared<detail::nothing>(true)); }

// --------------------------------------------------------------- combinators

rule operator>>(const rule& a, const rule& b)
{
    std::vector<expr::ptr> parts;

    // Flatten, so a >> b >> c is one node rather than two nested ones.  It
    // makes to_abnf() read the way it was written, and it keeps the step count
    // proportional to the grammar rather than to how it was parenthesized.
    const detail::concat* ca = dynamic_cast<const detail::concat*>(a.node().get());
    const detail::concat* cb = dynamic_cast<const detail::concat*>(b.node().get());

    if(ca) parts = ca->parts();
    else   parts.push_back(a.node());

    if(cb) {
        for(const expr::ptr& p : cb->parts()) parts.push_back(p);
    }
    else {
        parts.push_back(b.node());
    }

    return rule(std::make_shared<detail::concat>(std::move(parts)));
}

rule operator|(const rule& a, const rule& b)
{
    std::vector<expr::ptr> branches;

    const detail::alternation* aa =
        dynamic_cast<const detail::alternation*>(a.node().get());
    const detail::alternation* ab =
        dynamic_cast<const detail::alternation*>(b.node().get());

    if(aa) branches = aa->branches();
    else   branches.push_back(a.node());

    if(ab) {
        for(const expr::ptr& p : ab->branches()) branches.push_back(p);
    }
    else {
        branches.push_back(b.node());
    }

    return rule(std::make_shared<detail::alternation>(std::move(branches)));
}

rule operator*(const rule& a) { return rep(a, 0, rule::unbounded); }
rule operator+(const rule& a) { return rep(a, 1, rule::unbounded); }
rule operator-(const rule& a) { return rep(a, 0, 1); }
rule opt(const rule& a) { return rep(a, 0, 1); }

rule rep(const rule& a, std::size_t n) { return rep(a, n, n); }

rule rep(const rule& a, std::size_t min, std::size_t max)
{
    if(min > max) {
        throw grammar_error("rep(): minimum " + std::to_string(min) +
                            " is above maximum " + std::to_string(max));
    }

    return rule(std::make_shared<detail::repeat>(a.node(), min, max));
}

rule seq(std::initializer_list<rule> rs)
{
    std::vector<expr::ptr> parts;
    for(const rule& r : rs) parts.push_back(r.node());

    return rule(std::make_shared<detail::concat>(std::move(parts)));
}

rule alt(std::initializer_list<rule> rs)
{
    std::vector<expr::ptr> branches;
    for(const rule& r : rs) branches.push_back(r.node());

    return rule(std::make_shared<detail::alternation>(std::move(branches)));
}

rule as(std::string name, const rule& a)
{
    return rule(std::make_shared<detail::capture>(std::move(name), a.node()));
}

// ------------------------------------------------- context sensitive pieces

static std::size_t decimal(std::string_view s)
{
    std::size_t n = 0;

    for(char c : s) {
        if(c < '0' || c > '9') break;
        n = n * 10 + static_cast<std::size_t>(c - '0');
    }

    return n;
}

rule counted(const rule& source)
{
    return counted(source, decimal);
}

rule counted(const rule& source, const rule& element)
{
    return counted(source, element, decimal);
}

rule counted(const rule& source, std::function<std::size_t(std::string_view)> adapt)
{
    return rule(std::make_shared<detail::counted_node>(
                    source.node(), expr::ptr(), std::move(adapt)));
}

rule counted(const rule& source, const rule& element,
             std::function<std::size_t(std::string_view)> adapt)
{
    return rule(std::make_shared<detail::counted_node>(
                    source.node(), element.node(), std::move(adapt)));
}

rule backref(const rule& source) { return backref(source, false); }

rule backref(const rule& source, bool fold)
{
    return rule(std::make_shared<detail::backref_node>(source.node(), fold));
}

rule until(const rule& terminator)
{
    return rule(std::make_shared<detail::until_node>(terminator.node()));
}

rule where(std::string description,
           std::function<bool(std::string_view, std::size_t&)> f)
{
    return rule(std::make_shared<detail::predicate>(
                    std::move(description), std::move(f), false));
}

rule where_pure(std::string description,
                std::function<bool(std::string_view, std::size_t&)> f)
{
    return rule(std::make_shared<detail::predicate>(
                    std::move(description), std::move(f), true));
}


// ------------------------------------------------------------------- grammar

/**
 * ABNF rulenames are case-insensitive (RFC 5234 2.1).
 *
 * Deliberately not util::iequals, which upper-cases both arguments into
 * temporaries: this runs once per reference resolved and once per lookup, and
 * two allocations per comparison is not what it should cost.
 */
struct iless {
    typedef void is_transparent;

    bool operator()(std::string_view a, std::string_view b) const
    {
        const std::size_t n = std::min(a.size(), b.size());

        for(std::size_t i = 0; i < n; i++) {
            const unsigned char x =
                static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
            const unsigned char y =
                static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));

            if(x != y) return x < y;
        }

        return a.size() < b.size();
    }
};

class grammar::impl {
public:
    std::map<std::string, std::shared_ptr<slot>, iless> slots;
    std::vector<std::string> order;

    // Set by compile() only; see grammar::prose_rules().
    std::vector<std::string> prose;

    std::shared_ptr<slot> cell(std::string_view name)
    {
        auto i = slots.find(name);
        if(i != slots.end()) return i->second;

        std::shared_ptr<slot> s = std::make_shared<slot>();
        s->name = std::string(name);
        slots.emplace(s->name, s);

        return s;
    }
};

grammar::grammar() : m_impl(new impl) {}

grammar::grammar(grammar&&) noexcept = default;
grammar& grammar::operator=(grammar&&) noexcept = default;

grammar::~grammar()
{
    if(!m_impl) return;

    // Break the cycles before letting go.
    //
    // A recursive rule is a slot holding a body that holds a reference that
    // holds the slot.  Reference counting cannot see round that, so every
    // recursive grammar would leak its whole rule tree.  Clearing the bodies
    // cuts each cycle exactly once.
    for(auto& kv : m_impl->slots) {
        kv.second->body.reset();
    }
}

rule grammar::operator[](std::string_view name)
{
    return rule(std::make_shared<detail::reference>(m_impl->cell(name)));
}

rule grammar::at(std::string_view name) const
{
    auto i = m_impl->slots.find(name);

    if(i == m_impl->slots.end()) {
        throw grammar_error("no rule named \"" + std::string(name) + "\"");
    }

    return rule(std::make_shared<detail::reference>(i->second));
}

bool grammar::has(std::string_view name) const
{
    auto i = m_impl->slots.find(name);

    return i != m_impl->slots.end() && i->second->body;
}

void grammar::define(std::string name, const rule& body)
{
    std::shared_ptr<slot> s = m_impl->cell(name);

    if(s->body && !s->seeded) {
        throw grammar_error("rule \"" + s->name + "\" is already defined; use"
                            " define_alternative() for ABNF's \"=/\"");
    }

    if(!s->body) {
        m_impl->order.push_back(s->name);
    }

    s->seeded = false;
    s->body = body.node();
}

void grammar::define_alternative(std::string name, const rule& more)
{
    auto i = m_impl->slots.find(name);

    if(i == m_impl->slots.end() || !i->second->body) {
        throw grammar_error("rule \"" + std::string(name) + "\" has no"
                            " definition to add an alternative to");
    }

    rule(std::make_shared<detail::reference>(i->second)).define_alternative(more);
}

void grammar::seed_core()
{
    struct seed { const char* name; const rule& (*get)(); };

    static const seed cores[] = {
        { "ALPHA",  core::ALPHA  }, { "BIT",    core::BIT    },
        { "CHAR",   core::CHAR   }, { "CR",     core::CR     },
        { "CRLF",   core::CRLF   }, { "CTL",    core::CTL    },
        { "DIGIT",  core::DIGIT  }, { "DQUOTE", core::DQUOTE },
        { "HEXDIG", core::HEXDIG }, { "HTAB",   core::HTAB   },
        { "LF",     core::LF     }, { "LWSP",   core::LWSP   },
        { "OCTET",  core::OCTET  }, { "SP",     core::SP     },
        { "VCHAR",  core::VCHAR  }, { "WSP",    core::WSP    },
    };

    for(const seed& c : cores) {
        std::shared_ptr<slot> s = m_impl->cell(c.name);

        if(s->body) continue;

        s->body = c.get().node();
        s->seeded = true;
    }

    // Deliberately not added to m_impl->order: a core rule nothing referenced
    // is not part of the grammar anyone wrote, and printing sixteen of them
    // above every to_abnf() would bury it.
}

std::vector<std::string> grammar::rules() const { return m_impl->order; }

std::vector<std::string> grammar::prose_rules() const { return m_impl->prose; }

/** Everything reachable from e, following references into their bodies. */
static void reachable(const expr* e,
                      std::set<const expr*>& nodes,
                      std::set<const slot*>& cells,
                      std::vector<const expr*>& out)
{
    if(!e || nodes.count(e)) return;

    nodes.insert(e);
    out.push_back(e);

    const detail::reference* r = dynamic_cast<const detail::reference*>(e);

    if(r) {
        if(cells.count(r->cell().get())) return;

        cells.insert(r->cell().get());

        if(r->cell()->body) {
            reachable(r->cell()->body.get(), nodes, cells, out);
        }

        return;
    }

    std::vector<const expr*> kids;
    e->kids(kids);

    for(const expr* k : kids) reachable(k, nodes, cells, out);
}

std::vector<std::string> grammar::undefined() const
{
    std::set<const expr*> nodes;
    std::set<const slot*> cells;
    std::vector<const expr*> all;

    for(const std::string& n : m_impl->order) {
        auto i = m_impl->slots.find(n);
        if(i != m_impl->slots.end() && i->second->body) {
            reachable(i->second->body.get(), nodes, cells, all);
        }
    }

    std::vector<std::string> out;

    for(const expr* e : all) {
        const detail::reference* r = dynamic_cast<const detail::reference*>(e);

        if(r && !r->cell()->body) {
            if(std::find(out.begin(), out.end(), r->cell()->name) == out.end()) {
                out.push_back(r->cell()->name);
            }
        }
    }

    return out;
}

void grammar::check() const
{
    // Every undefined rule at once.  A grammar pasted out of an RFC is usually
    // missing several, and reporting them one run at a time is miserable.
    const std::vector<std::string> missing = undefined();

    if(!missing.empty()) {
        std::string m = "referenced but never defined:";

        for(const std::string& n : missing) m += " " + n;

        throw grammar_error(m);
    }

    // Left recursion.  A rule that can reach itself without consuming anything
    // recurses forever; the depth guard would eventually stop it, but with a
    // message about depth rather than about the grammar.
    for(const std::string& n : m_impl->order) {
        auto i = m_impl->slots.find(n);
        if(i == m_impl->slots.end() || !i->second->body) continue;

        std::set<const slot*> out, visiting;
        visiting.insert(i->second.get());
        i->second->body->leftmost(out, visiting);

        if(out.count(i->second.get())) {
            throw grammar_error("left recursion through rule \"" + n + "\"");
        }
    }

    // A repetition of something that can match nothing never terminates.
    std::set<const expr*> nodes;
    std::set<const slot*> cells;
    std::vector<const expr*> all;

    for(const std::string& n : m_impl->order) {
        auto i = m_impl->slots.find(n);
        if(i != m_impl->slots.end() && i->second->body) {
            reachable(i->second->body.get(), nodes, cells, all);
        }
    }

    for(const expr* e : all) {
        const detail::repeat* r = dynamic_cast<const detail::repeat*>(e);

        if(!r || r->max() != rule::unbounded) continue;

        std::set<const slot*> v;

        if(r->body()->nullable(v)) {
            throw grammar_error("unbounded repetition of something that can"
                                " match the empty string");
        }
    }
}

std::string grammar::to_abnf() const
{
    std::ostringstream o;

    for(const std::string& n : m_impl->order) {
        auto i = m_impl->slots.find(n);
        if(i == m_impl->slots.end() || !i->second->body) continue;

        o << n << " = ";
        i->second->body->write(o, detail::PREC_ALT);
        o << "\r\n";
    }

    return o.str();
}


// ------------------------------------------------------------- the ABNF text

std::string dedent(std::string_view text)
{
    std::size_t common = std::string::npos;
    std::size_t i = 0;

    while(i <= text.size()) {
        std::size_t eol = text.find('\n', i);
        if(eol == std::string_view::npos) eol = text.size();

        std::size_t j = i;
        while(j < eol && (text[j] == ' ' || text[j] == '\t')) j++;

        // Blank lines say nothing about the common indent.
        if(j < eol && (common == std::string::npos || j - i < common)) {
            common = j - i;
        }

        if(eol == text.size()) break;
        i = eol + 1;
    }

    if(common == std::string::npos || common == 0) {
        return std::string(text);
    }

    std::string out;
    i = 0;

    while(i <= text.size()) {
        std::size_t eol = text.find('\n', i);
        const bool last = (eol == std::string_view::npos);
        if(last) eol = text.size();

        std::size_t skip = 0;
        while(skip < common && i + skip < eol &&
              (text[i + skip] == ' ' || text[i + skip] == '\t')) {
            skip++;
        }

        out.append(text.substr(i + skip, eol - (i + skip)));

        if(last) break;

        out += '\n';
        i = eol + 1;
    }

    return out;
}

namespace {

/**
 * RFC 5234 section 4, written in the combinators of section... this file.
 *
 * The bootstrap: ABNF's own grammar, by hand, once -- and everything after it
 * is read rather than written.  It is also the strongest test available, since
 * the result can be asked to parse the text it was built from.
 *
 * Two deliberate departures from the RFC's own text, both noted where they
 * occur: repeat's alternatives are reordered, because ordered choice would
 * commit to the wrong one, and c-nl may be a bare LF, because grammar text
 * pasted out of a browser is not CRLF.
 */
class abnf_grammar {
public:
    explicit abnf_grammar(bool bare_lf)
    {
        const rule ALPHA = core::ALPHA();
        const rule DIGIT = core::DIGIT();
        const rule HEXDIG = core::HEXDIG();
        const rule WSP = core::WSP();
        const rule VCHAR = core::VCHAR();
        const rule DQUOTE = core::DQUOTE();

        // RFC 5234 says CRLF.  Text pasted from a browser or an RFC .txt file
        // has bare LFs, and refusing it would defeat the point of reading ABNF
        // text at all.
        const rule nl = bare_lf
            ? (core::CRLF() | core::LF() | core::CR())
            : core::CRLF();

        const rule comment = lit(";") >> *(WSP | VCHAR) >> nl;
        const rule c_nl = comment | nl;
        const rule c_wsp = WSP | (c_nl >> WSP);

        const rule rulename = ALPHA >> *(ALPHA | DIGIT | lit("-"));

        // char-val, with RFC 7405's %s and %i.  Case insensitive by default,
        // per 5234 2.3 -- which is the opposite of lit() and is why the two
        // layers differ there.
        const rule quoted =
            DQUOTE >> as("cv-text", *(rng(0x20, 0x21) | rng(0x23, 0x7E)))
                   >> DQUOTE;

        const rule char_val = as("char-val",
            (as("cv-sensitive", ilit("%s")) >> quoted) |
            (-ilit("%i") >> quoted));

        // The body is taken whole and read in C++: splitting 1*BIT from
        // 1*DIGIT from 1*HEXDIG in the grammar buys nothing when the value has
        // to be converted anyway, and the base is right there.
        const rule num_val = as("num-val",
            lit("%") >> as("nv-base", anyof("bdxBDX"))
                     >> as("nv-body", +(HEXDIG | anyof(".-"))));

        const rule prose_val = as("prose-val",
            lit("<") >> as("prose-text", *(rng(0x20, 0x3D) | rng(0x3F, 0x7E)))
                     >> lit(">"));

        m_grammar.define("element",
            as("ref", rulename)
          | as("group",  lit("(") >> *c_wsp >> m_grammar["alternation"]
                                  >> *c_wsp >> lit(")"))
          | as("option", lit("[") >> *c_wsp >> m_grammar["alternation"]
                                  >> *c_wsp >> lit("]"))
          | char_val | num_val | prose_val);

        // RFC 5234 writes this as
        //
        //     repeat = 1*DIGIT / (*DIGIT "*" *DIGIT)
        //
        // which cannot work under ordered choice: against "3*5" the first
        // alternative takes the 3 and the enclosing rule then fails on the
        // "*", with the choice already committed.  Longest first.
        m_grammar.define("repetition",
            as("repetition",
               -as("repeat", (*DIGIT >> lit("*") >> *DIGIT) | +DIGIT)
               >> m_grammar["element"]));

        m_grammar.define("concatenation",
            as("concatenation",
               m_grammar["repetition"] >> *(+c_wsp >> m_grammar["repetition"])));

        m_grammar.define("alternation",
            as("alternation",
               m_grammar["concatenation"]
               >> *(*c_wsp >> lit("/") >> *c_wsp >> m_grammar["concatenation"])));

        // "=/" before "=", or the choice commits to "=" and leaves a stray "/".
        m_grammar.define("rule",
            as("rule",
               as("defined-name", rulename) >> *c_wsp
               >> as("op", lit("=/") | lit("=")) >> *c_wsp
               >> m_grammar["alternation"] >> *c_wsp >> c_nl));

        m_grammar.define("rulelist",
            +( m_grammar["rule"] | (*c_wsp >> c_nl) ));

        m_grammar.check();
    }

    rule rulelist() const { return m_grammar.at("rulelist"); }

protected:
    mutable grammar m_grammar;
};

const abnf_grammar& bootstrap(bool bare_lf)
{
    static const abnf_grammar strict(false);
    static const abnf_grammar loose(true);

    return bare_lf ? loose : strict;
}

// ------------------------------------------------------ reading the tree back

rule build_alternation(const match& m, grammar& g, const compile_options& o);

unsigned long digits_of(std::string_view s, int base)
{
    unsigned long v = 0;

    for(char c : s) {
        int d;

        if(c >= '0' && c <= '9')      d = c - '0';
        else if(c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if(c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else throw grammar_error(std::string("\"") + c +
                                 "\" is not a digit in that base");

        if(d >= base) {
            throw grammar_error(std::string("\"") + c +
                                "\" is not a digit in base " +
                                std::to_string(base));
        }

        v = v * static_cast<unsigned long>(base) + static_cast<unsigned long>(d);

        if(v > 0xFF && base != 0) {
            // Checked as it accumulates, so a long run of digits cannot wrap.
            // ABNF is octet based; a value that does not fit in one is a
            // mistake in the grammar rather than something to truncate.
            throw grammar_error("numeric value above %xFF; ABNF is octets");
        }
    }

    return v;
}

rule build_num_val(const match& m)
{
    const std::string base_text = m.child("nv-base").str();
    const std::string body = m.child("nv-body").str();

    const char b = static_cast<char>(std::tolower(
        static_cast<unsigned char>(base_text[0])));

    const int base = (b == 'b') ? 2 : (b == 'd') ? 10 : 16;

    const std::size_t dash = body.find('-');

    if(dash != std::string::npos) {
        const unsigned long lo = digits_of(body.substr(0, dash), base);
        const unsigned long hi = digits_of(body.substr(dash + 1), base);

        return rng(static_cast<unsigned char>(lo), static_cast<unsigned char>(hi));
    }

    // A dotted run is a sequence of octets, and never case folded.
    std::string bytes_out;
    std::size_t at = 0;

    while(at <= body.size()) {
        std::size_t dot = body.find('.', at);
        if(dot == std::string::npos) dot = body.size();

        bytes_out += static_cast<char>(digits_of(body.substr(at, dot - at), base));

        if(dot == body.size()) break;
        at = dot + 1;
    }

    if(bytes_out.size() == 1) {
        return chr(static_cast<unsigned char>(bytes_out[0]));
    }

    return lit(bytes_out);
}

rule build_element(const match& m, grammar& g, const compile_options& o)
{
    const std::string k = m.name();

    if(k == "ref") {
        return g[m.str()];
    }

    if(k == "group") {
        return build_alternation(m.child("alternation"), g, o);
    }

    if(k == "option") {
        return opt(build_alternation(m.child("alternation"), g, o));
    }

    if(k == "char-val") {
        const match t = m.child("cv-text");
        const std::string text = t ? t.str() : std::string();

        // RFC 5234 2.3: a quoted string is case insensitive.  RFC 7405's %s
        // is the way to ask for the other thing.
        return m.child("cv-sensitive") ? lit(text) : ilit(text);
    }

    if(k == "num-val") {
        return build_num_val(m);
    }

    if(k == "prose-val") {
        const match t = m.child("prose-text");
        const std::string text = t ? t.str() : std::string();

        if(o.prose) {
            return o.prose(text);
        }

        // Compiles; fails when reached.  Refusing to compile a grammar because
        // one obsolete production is prose would defeat the purpose of being
        // able to paste one in.
        return where("prose <" + text + ">",
                     [text](std::string_view, std::size_t&) -> bool {
                         throw grammar_error("prose-val <" + text + "> has no "
                                             "implementation; supply one with "
                                             "compile_options::prose");
                     });
    }

    throw grammar_error("unrecognised element \"" + k + "\" in the grammar text");
}

rule build_repetition(const match& m, grammar& g, const compile_options& o)
{
    match repeat, element;

    for(const match& c : m.children()) {
        if(c.name() == "repeat") repeat = c;
        else                     element = c;
    }

    if(!element) {
        throw grammar_error("a repetition with nothing to repeat");
    }

    rule e = build_element(element, g, o);

    if(!repeat) {
        return e;
    }

    const std::string r = repeat.str();
    const std::size_t star = r.find('*');

    if(star == std::string::npos) {
        const unsigned long n = digits_of(r, 10);

        return rep(e, n, n);
    }

    const std::string lo = r.substr(0, star);
    const std::string hi = r.substr(star + 1);

    return rep(e,
               lo.empty() ? 0 : digits_of(lo, 10),
               hi.empty() ? rule::unbounded : digits_of(hi, 10));
}

rule build_concatenation(const match& m, grammar& g, const compile_options& o)
{
    rule out;
    bool first = true;

    for(const match& c : m.children()) {
        if(c.name() != "repetition") continue;

        rule r = build_repetition(c, g, o);

        out = first ? r : (out >> r);
        first = false;
    }

    return first ? empty() : out;
}

rule build_alternation(const match& m, grammar& g, const compile_options& o)
{
    rule out;
    bool first = true;

    for(const match& c : m.children()) {
        if(c.name() != "concatenation") continue;

        rule r = build_concatenation(c, g, o);

        out = first ? r : (out | r);
        first = false;
    }

    return first ? empty() : out;
}

}  // namespace

grammar compile(std::string_view text)
{
    return compile(text, compile_options());
}

grammar compile(std::string_view text, const compile_options& o)
{
    std::string src = o.dedent ? dedent(text) : std::string(text);

    // rulelist ends every rule with a c-nl, and grammar text routinely does
    // not end with a newline.  Supplying one is kinder than making every
    // caller remember.
    if(src.empty() || (src.back() != '\n' && src.back() != '\r')) {
        src += "\r\n";
    }

    grammar g;

    if(o.seed_core_rules) {
        g.seed_core();
    }

    options po;
    po.captures = options::capture_policy::named;

    const parse_result r = bootstrap(o.allow_bare_lf).rulelist().try_parse(src, po);

    if(!r) {
        const error& e = r.why();

        throw grammar_error("the grammar text does not parse at line " +
                            std::to_string(e.line()) + ", column " +
                            std::to_string(e.column()) + ": " + e.context_line());
    }

    for(const match& rl : r.root().all("rule")) {
        const std::string name = rl.child("defined-name").str();
        const std::string op = rl.child("op").str();

        // as(name, ...) rather than the bare body: a rule name is the only
        // name a grammar written in text has, so it has to be the capture
        // name too, or a compiled grammar can answer "does this parse" and
        // nothing else.  Costs nothing under capture_policy::listed, which is
        // what a grammar this size wants anyway.
        rule body = build_alternation(rl.child("alternation"), g, o);

        if(op == "=/") g.define_alternative(name, as(name, body));
        else           g.define(name, as(name, body));

        // Recorded rather than derived: once a prose-val has been lowered to a
        // rule there is nothing left in the tree that says it was one.
        if(!o.prose && !rl.all("prose-val").empty()) {
            g.m_impl->prose.push_back(name);
        }
    }

    return g;
}

// ----------------------------------------------------------------- core rules

namespace core {

const rule& ALPHA()
{
    static const rule r = rng(0x41, 0x5A) | rng(0x61, 0x7A);
    return r;
}

const rule& BIT()
{
    static const rule r = lit("0") | lit("1");
    return r;
}

const rule& CHAR()
{
    static const rule r = rng(0x01, 0x7F);
    return r;
}

const rule& CR()
{
    static const rule r = chr(0x0D);
    return r;
}

const rule& CRLF()
{
    static const rule r = CR() >> LF();
    return r;
}

const rule& CTL()
{
    static const rule r = rng(0x00, 0x1F) | chr(0x7F);
    return r;
}

const rule& DIGIT()
{
    static const rule r = rng(0x30, 0x39);
    return r;
}

const rule& DQUOTE()
{
    static const rule r = chr(0x22);
    return r;
}

const rule& HEXDIG()
{
    static const rule r = DIGIT() | anyof("ABCDEFabcdef");
    return r;
}

const rule& HTAB()
{
    static const rule r = chr(0x09);
    return r;
}

const rule& LF()
{
    static const rule r = chr(0x0A);
    return r;
}

const rule& LWSP()
{
    // Included because Appendix B defines it, with the warning the appendix
    // itself carries: this accepts a bare CRLF with nothing after it, which is
    // almost never what a grammar wants.  Prefer WSP.
    static const rule r = *(WSP() | (CRLF() >> WSP()));
    return r;
}

const rule& OCTET()
{
    static const rule r = rng(0x00, 0xFF);
    return r;
}

const rule& SP()
{
    static const rule r = chr(0x20);
    return r;
}

const rule& VCHAR()
{
    static const rule r = rng(0x21, 0x7E);
    return r;
}

const rule& WSP()
{
    static const rule r = SP() | HTAB();
    return r;
}

}  // namespace core

}
}
}
