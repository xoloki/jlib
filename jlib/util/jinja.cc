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

#include <jlib/util/jinja.hh>

#include <cctype>
#include <cstdlib>

namespace jlib {
namespace util {
namespace jinja {

/**
 * The grammar, with the two text rules supplied in combinators.
 *
 * "Everything up to the next tag" cannot be written in ABNF here, because
 * repetition is possessive: `*CHAR "{{"` matches to the end of the input and
 * then has no braces left to match.  until() is that rule, and this is the
 * same shape as RFC 3501's `literal`, which counted() supplies in
 * jlib/net/imap_response.cc for the same reason.
 *
 * check() runs after the definitions, not before -- an undefined rule is only
 * an error once everything that was going to define one has.
 */
const abnf::grammar& grammar()
{
    static const abnf::grammar& g = [] () -> const abnf::grammar& {
        static abnf::grammar h = [] {
            abnf::grammar h = abnf::compile(GRAMMAR);

            // Text runs to the next tag *or to the end of the template*,
            // and that second half is why this is a predicate rather than
            // until(): until() requires its terminator to be present, so a
            // template ending in ordinary text -- which most do -- failed on
            // the last run of characters. The rule reads "up to the next
            // opener" either way; only the end-of-input case differs.
            //
            // Requiring at least one byte is what keeps *node terminating.
            // An alternative that can match empty makes the repetition
            // progress without consuming, and the parse never ends.
            h.define("raw-text", abnf::as("raw-text", abnf::where_pure(
                "text up to the next tag",
                [](std::string_view in, std::size_t& at) {
                    const std::size_t start = at;

                    while(at < in.size()) {
                        // A lone "{" is ordinary text; only "{{", "{%" and
                        // "{#" open a tag. At the very end of the input there
                        // is no next byte to look at, so it is text too.
                        if(in[at] == '{' && at + 1 < in.size() &&
                           (in[at + 1] == '{' || in[at + 1] == '%' ||
                            in[at + 1] == '#'))
                            break;

                        at++;
                    }

                    return at > start;
                })));

            h.define("comment-text",
                     abnf::as("comment-text", abnf::until(abnf::lit("#}"))));

            h.check();

            return h;
        }();

        return h;
    }();

    return g;
}

}
}
}

namespace jlib {
namespace util {
namespace jinja {

// --------------------------------------------------------------------- text

std::string flatten(const text& t)
{
    std::string out;

    for(std::size_t i = 0; i < t.size(); i++) out += t[i].text;

    return out;
}

text literal_text(std::string s)
{
    text t;

    if(!s.empty()) t.push_back(span{ std::move(s), true });

    return t;
}

// -------------------------------------------------------------------- value

value::value() : m_kind(kind::none), m_bool(false), m_number(0) {}
value::value(bool b) : m_kind(kind::boolean), m_bool(b), m_number(0) {}
value::value(long n) : m_kind(kind::number), m_bool(false), m_number(n) {}

value::value(std::string s, bool literal)
    : m_kind(kind::string), m_bool(false), m_number(0)
{
    if(!s.empty()) m_text.push_back(span{ std::move(s), literal });
}

value::value(text t)
    : m_kind(kind::string), m_bool(false), m_number(0), m_text(std::move(t)) {}

value value::of(std::vector<value> items)
{
    value v;

    v.m_kind = kind::list;
    v.m_items = std::move(items);

    return v;
}

value value::of(std::map<std::string, value> fields)
{
    value v;

    v.m_kind = kind::map;
    v.m_fields = std::move(fields);

    return v;
}

bool value::truthy() const
{
    switch(m_kind) {
    case kind::none:    return false;
    case kind::boolean: return m_bool;
    case kind::number:  return m_number != 0;
    case kind::string:  return !flat().empty();
    case kind::list:    return !m_items.empty();
    case kind::map:     return !m_fields.empty();
    }

    return false;
}

const text& value::str() const
{
    if(m_kind != kind::string) throw exception("not a string");

    return m_text;
}

std::string value::flat() const
{
    if(m_kind == kind::string) return flatten(m_text);
    if(m_kind == kind::number) return std::to_string(m_number);
    if(m_kind == kind::boolean) return m_bool ? "True" : "False";
    if(m_kind == kind::none) return "";

    throw exception("a list or a map has no text");
}

long value::number() const
{
    if(m_kind != kind::number) throw exception("not a number");

    return m_number;
}

const std::vector<value>& value::items() const
{
    if(m_kind != kind::list) throw exception("not a list");

    return m_items;
}

const std::map<std::string, value>& value::fields() const
{
    if(m_kind != kind::map) throw exception("not a map");

    return m_fields;
}

bool value::has(const std::string& key) const
{
    return m_kind == kind::map && m_fields.find(key) != m_fields.end();
}

const value& value::at(const std::string& key) const
{
    std::map<std::string, value>::const_iterator i = m_fields.find(key);

    if(m_kind != kind::map || i == m_fields.end())
        throw exception("no such field '" + key + "'");

    return i->second;
}

bool value::operator==(const value& o) const
{
    if(m_kind != o.m_kind) {
        // A template compares a missing field against a string often enough
        // that this has to answer rather than throw: none equals nothing.
        if(m_kind == kind::none || o.m_kind == kind::none) return false;

        return false;
    }

    switch(m_kind) {
    case kind::none:    return true;
    case kind::boolean: return m_bool == o.m_bool;
    case kind::number:  return m_number == o.m_number;
    case kind::string:  return flat() == o.flat();
    case kind::list:    return m_items == o.m_items;
    case kind::map:     return m_fields == o.m_fields;
    }

    return false;
}

}
}
}

namespace jlib {
namespace util {
namespace jinja {

namespace {

/** Immediate children with this name.  all() would reach into nested blocks. */
abnf::match::list direct(const abnf::match& m, const char* name)
{
    abnf::match::list out;
    const abnf::match::list kids = m.children();

    for(std::size_t i = 0; i < kids.size(); i++)
        if(kids[i].name() == name) out.push_back(kids[i]);

    return out;
}

/** Names bound while rendering: the context, plus set and loop variables. */
typedef std::map<std::string, value> scope;

std::string trim_ws(const std::string& s)
{
    std::size_t b = 0, e = s.size();

    while(b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) b++;
    while(e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\n' || s[e-1] == '\r')) e--;

    return s.substr(b, e - b);
}

/** A quoted literal's characters, with the backslash escapes resolved. */
std::string unquote(const std::string& lit)
{
    if(lit.size() < 2) return std::string();

    const std::string body = lit.substr(1, lit.size() - 2);
    std::string out;

    for(std::size_t i = 0; i < body.size(); i++) {
        if(body[i] != '\\' || i + 1 == body.size()) { out += body[i]; continue; }

        switch(body[++i]) {
        case 'n':  out += '\n'; break;
        case 't':  out += '\t'; break;
        case 'r':  out += '\r'; break;
        case '\\': out += '\\'; break;
        case '\'': out += '\''; break;
        case '"':  out += '"';  break;
        default:   out += '\\'; out += body[i]; break;
        }
    }

    return out;
}

/**
 * Template text, with the whitespace rules applied.
 *
 * Decided from the source either side of the run rather than from its
 * neighbours in the tree, because a run's neighbours may be in a different
 * block -- the text before {% endif %} is the if-body's last node, and the
 * tag that trims it belongs to the block above.  The bytes are unambiguous
 * and always there.
 *
 * Four rules, in precedence order.  An explicit dash beats the defaults, and
 * the defaults are transformers' rather than Jinja's: see jinja.hh.
 */
std::string layout(const std::string& src, std::size_t b, std::size_t e)
{
    std::string t = src.substr(b, e - b);

    // What comes before: "-%}" strips all leading whitespace, a plain block
    // close strips one newline (trim_blocks), an output close strips nothing.
    const bool dash_before = b >= 3 && src[b-3] == '-' &&
        (src.compare(b-2, 2, "%}") == 0 || src.compare(b-2, 2, "#}") == 0 ||
         src.compare(b-2, 2, "}}") == 0);

    const bool block_before = !dash_before && b >= 2 &&
        (src.compare(b-2, 2, "%}") == 0 || src.compare(b-2, 2, "#}") == 0);

    if(dash_before) {
        std::size_t i = 0;

        while(i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) i++;

        t.erase(0, i);
    }
    else if(block_before) {
        if(t.compare(0, 2, "\r\n") == 0) t.erase(0, 2);
        else if(!t.empty() && t[0] == '\n') t.erase(0, 1);
    }

    // What comes after: "{%-" strips all trailing whitespace, a plain block
    // open strips the run of spaces and tabs back to the line start
    // (lstrip_blocks) but leaves the newline that began the line.
    const bool dash_after = e + 2 < src.size() && src[e+2] == '-' &&
        (src.compare(e, 2, "{%") == 0 || src.compare(e, 2, "{#") == 0 ||
         src.compare(e, 2, "{{") == 0);

    const bool block_after = !dash_after && e + 1 < src.size() &&
        (src.compare(e, 2, "{%") == 0 || src.compare(e, 2, "{#") == 0);

    if(dash_after) {
        std::size_t i = t.size();

        while(i > 0 && std::isspace(static_cast<unsigned char>(t[i-1]))) i--;

        t.erase(i);
    }
    else if(block_after) {
        std::size_t i = t.size();

        while(i > 0 && (t[i-1] == ' ' || t[i-1] == '\t')) i--;

        if(i == 0 || t[i-1] == '\n') t.erase(i);
    }

    return t;
}

// The evaluator is mutually recursive: an expression may contain a block's
// variable and a block's body contains expressions.
value eval(const abnf::match& m, const scope& s);
void render_nodes(const std::string& src, const abnf::match& node,
                  scope& s, text& out);

const value& look_up(const std::string& name, const scope& s)
{
    static const value nothing;
    const scope::const_iterator i = s.find(name);

    return i == s.end() ? nothing : i->second;
}

value eval_atom(const abnf::match& m, const scope& s)
{
    if(const abnf::match str = m.child("string"))
        return value(unquote(std::string(str.text())), true);

    if(const abnf::match n = m.child("number"))
        return value(static_cast<long>(std::atol(std::string(n.text()).c_str())));

    if(const abnf::match b = m.child("boolean")) {
        const std::string t = std::string(b.text());

        return value(t == "true" || t == "True");
    }

    if(m.child("none")) return value();
    if(const abnf::match g = m.child("group")) return eval(g.child("expr"), s);
    if(const abnf::match n = m.child("name")) return look_up(std::string(n.text()), s);

    throw tmpl::exception("an expression this does not understand");
}

value eval_postfix(const abnf::match& m, const scope& s)
{
    const abnf::match::list kids = m.children();

    if(kids.empty()) throw tmpl::exception("an empty expression");

    value v = eval_atom(kids[0], s);

    // The name of the thing being called, kept so that raise_exception can be
    // recognised at the point its arguments arrive.
    std::string called = kids[0].child("name")
        ? std::string(kids[0].child("name").text()) : std::string();

    for(std::size_t i = 1; i < kids.size(); i++) {
        const std::string what = kids[i].name();

        if(what == "attribute") {
            const std::string field = std::string(kids[i].child("name").text());

            v = v.type() == value::kind::map && v.has(field) ? v.at(field) : value();
            called.clear();
        }
        else if(what == "subscript") {
            const value key = eval(kids[i].child("expr"), s);

            if(v.type() == value::kind::list) {
                // A string key on a list is nothing, not element zero.  A
                // missing map field already answers none; a list has to give
                // the same answer or "items['nope']" quietly yields items[0].
                if(key.type() != value::kind::number) v = value();
                else {
                    const long n = key.number();

                    v = n >= 0 && std::size_t(n) < v.items().size()
                        ? v.items()[std::size_t(n)] : value();
                }
            }
            else {
                const std::string k = key.flat();

                v = v.type() == value::kind::map && v.has(k) ? v.at(k) : value();
            }

            called.clear();
        }
        else if(what == "call-args") {
            // The one call a chat template makes.  Anything else would have
            // to be implemented rather than guessed at, so it says so.
            if(called != "raise_exception")
                throw tmpl::exception("this template calls '" +
                                      (called.empty() ? std::string("something")
                                                      : called) +
                                      "', and only raise_exception() is implemented");

            const abnf::match args = kids[i].child("arg-list");
            const std::string why = args ? eval(direct(args, "expr")[0], s).flat()
                                         : std::string();

            throw tmpl::exception("the template refused: " + why);
        }
    }

    return v;
}

value apply_filter(const value& v, const abnf::match& f)
{
    const std::string name = std::string(f.child("name").text());

    if(name == "trim") {
        // Trimming a span list has to keep the spans: the characters go, the
        // provenance of what is left does not change.
        text t = v.type() == value::kind::string ? v.str()
                                                 : literal_text(v.flat());

        while(!t.empty()) {
            std::string& front = t.front().text;
            std::size_t i = 0;

            while(i < front.size() &&
                  std::isspace(static_cast<unsigned char>(front[i]))) i++;

            front.erase(0, i);

            if(!front.empty()) break;

            t.erase(t.begin());
        }

        while(!t.empty()) {
            std::string& back = t.back().text;
            std::size_t i = back.size();

            while(i > 0 && std::isspace(static_cast<unsigned char>(back[i-1]))) i--;

            back.erase(i);

            if(!back.empty()) break;

            t.pop_back();
        }

        return value(t);
    }

    throw tmpl::exception("this template uses the filter '" + name +
                          "', which is not implemented");
}

value eval(const abnf::match& m, const scope& s)
{
    const std::string what = m.name();

    if(what == "expr") return eval(m.child("or-expr"), s);

    // or and and yield an *operand*, not a boolean: "a or b" is the first
    // truthy one, else the last, which is what makes "{{ x or 'default' }}"
    // the idiom it is.  Returning true/false works for every use inside an
    // if -- which is why a test suite that only ever puts them there will not
    // notice.
    if(what == "or-expr") {
        const abnf::match::list parts = direct(m, "and-expr");

        if(parts.size() == 1) return eval(parts[0], s);

        value last;

        for(std::size_t i = 0; i < parts.size(); i++) {
            last = eval(parts[i], s);

            if(last.truthy()) return last;
        }

        return last;
    }

    if(what == "and-expr") {
        const abnf::match::list parts = direct(m, "not-expr");

        if(parts.size() == 1) return eval(parts[0], s);

        value last;

        for(std::size_t i = 0; i < parts.size(); i++) {
            last = eval(parts[i], s);

            if(!last.truthy()) return last;
        }

        return last;
    }

    if(what == "not-expr") {
        if(const abnf::match inner = m.child("not-expr"))
            return value(!eval(inner, s).truthy());

        return eval(m.child("comparison"), s);
    }

    if(what == "comparison") {
        const abnf::match::list parts = direct(m, "concat");

        if(parts.size() == 1) return eval(parts[0], s);

        const value l = eval(parts[0], s);
        const value r = eval(parts[1], s);
        const std::string op = trim_ws(std::string(m.child("compare-op").text()));

        if(op == "==") return value(l == r);
        if(op == "!=") return value(l != r);

        if(op == "in" || op == "not in") {
            bool found = false;

            if(r.type() == value::kind::list) {
                for(std::size_t i = 0; i < r.items().size() && !found; i++)
                    found = r.items()[i] == l;
            }
            else if(r.type() == value::kind::map) found = r.has(l.flat());
            else found = r.flat().find(l.flat()) != std::string::npos;

            return value(op == "in" ? found : !found);
        }

        // The remaining four order their operands.  Numbers compare as
        // numbers and strings compare as strings, which is what Python does;
        // anything else throws.
        //
        // It used to coerce a non-number to zero, under a comment saying the
        // case was not modelled -- which modelled it, as zero, in silence.
        // Saying a thing is unsupported and then quietly supporting it wrongly
        // is the failure this file exists to avoid.
        if(l.type() == value::kind::number && r.type() == value::kind::number) {
            const long a = l.number(), b = r.number();

            if(op == ">")  return value(a >  b);
            if(op == "<")  return value(a <  b);
            if(op == ">=") return value(a >= b);
            if(op == "<=") return value(a <= b);
        }

        if(l.type() == value::kind::string && r.type() == value::kind::string) {
            const std::string a = l.flat(), b = r.flat();

            if(op == ">")  return value(a >  b);
            if(op == "<")  return value(a <  b);
            if(op == ">=") return value(a >= b);
            if(op == "<=") return value(a <= b);
        }

        throw tmpl::exception("'" + op + "' between a " +
                              std::to_string(int(l.type())) + " and a " +
                              std::to_string(int(r.type())) +
                              " is not implemented");
    }

    if(what == "concat") {
        const abnf::match::list parts = direct(m, "filtered");

        if(parts.size() == 1) return eval(parts[0], s);

        // Concatenation is where provenance would be lost if a string were a
        // string: the spans of each part are kept, in order.
        text joined;

        for(std::size_t i = 0; i < parts.size(); i++) {
            const value v = eval(parts[i], s);
            const text t = v.type() == value::kind::string ? v.str()
                                                           : literal_text(v.flat());

            joined.insert(joined.end(), t.begin(), t.end());
        }

        return value(joined);
    }

    if(what == "filtered") {
        value v = eval_postfix(m.child("postfix"), s);
        const abnf::match::list filters = direct(m, "filter");

        for(std::size_t i = 0; i < filters.size(); i++) v = apply_filter(v, filters[i]);

        return v;
    }

    if(what == "postfix") return eval_postfix(m, s);

    throw tmpl::exception("an expression node this does not understand: " + what);
}

/** The loop variable Jinja exposes inside a for. */
value loop_state(std::size_t i, std::size_t n)
{
    std::map<std::string, value> f;

    f["index0"] = value(static_cast<long>(i));
    f["index"]  = value(static_cast<long>(i + 1));
    f["first"]  = value(i == 0);
    f["last"]   = value(i + 1 == n);
    f["length"] = value(static_cast<long>(n));

    return value::of(f);
}

void render_if(const std::string& src, const abnf::match& m,
               scope& s, text& out)
{
    // Children arrive in order: if-tag, its body, then each elif-part, then
    // any else-part.  The body of the "if" is whatever sits between the tag
    // and the first part that is not a node.
    const abnf::match::list kids = m.children();

    if(eval(kids[0].child("expr"), s).truthy()) {
        for(std::size_t i = 1; i < kids.size(); i++) {
            if(kids[i].name() != "node") break;

            render_nodes(src, kids[i], s, out);
        }

        return;
    }

    for(std::size_t i = 1; i < kids.size(); i++) {
        if(kids[i].name() == "elif-part") {
            if(!eval(kids[i].child("elif-tag").child("expr"), s).truthy()) continue;

            const abnf::match::list body = direct(kids[i], "node");

            for(std::size_t j = 0; j < body.size(); j++) render_nodes(src, body[j], s, out);

            return;
        }

        if(kids[i].name() == "else-part") {
            const abnf::match::list body = direct(kids[i], "node");

            for(std::size_t j = 0; j < body.size(); j++) render_nodes(src, body[j], s, out);

            return;
        }
    }
}

void render_for(const std::string& src, const abnf::match& m,
                scope& s, text& out)
{
    const abnf::match::list kids = m.children();
    const abnf::match tag = kids[0];
    const abnf::match::list names = direct(tag.child("name-list"), "name");
    const value seq = eval(tag.child("expr"), s);

    std::vector<value> items;

    if(seq.type() == value::kind::list) items = seq.items();
    else if(seq.type() == value::kind::map) {
        const std::map<std::string, value>& f = seq.fields();

        for(std::map<std::string, value>::const_iterator i = f.begin();
            i != f.end(); ++i)
            items.push_back(value(i->first, true));
    }

    if(items.empty()) {
        for(std::size_t i = 1; i < kids.size(); i++)
            if(kids[i].name() == "else-part") {
                const abnf::match::list body = direct(kids[i], "node");

                for(std::size_t j = 0; j < body.size(); j++)
                    render_nodes(src, body[j], s, out);
            }

        return;
    }

    for(std::size_t i = 0; i < items.size(); i++) {
        // A copy per iteration: Jinja scopes the loop variable to the block,
        // and a set inside the body does not escape it.
        scope inner = s;

        if(names.size() == 1) inner[std::string(names[0].text())] = items[i];
        else {
            // "for k, v in items" -- the item has to be a pair-shaped list.
            for(std::size_t k = 0; k < names.size(); k++)
                inner[std::string(names[k].text())] =
                    items[i].type() == value::kind::list &&
                    k < items[i].items().size() ? items[i].items()[k] : value();
        }

        inner["loop"] = loop_state(i, items.size());

        for(std::size_t j = 1; j < kids.size(); j++) {
            if(kids[j].name() != "node") continue;

            render_nodes(src, kids[j], inner, out);
        }
    }
}

/** One node: the single alternative that matched under it. */
void render_nodes(const std::string& src, const abnf::match& node,
                  scope& s, text& out)
{
    const abnf::match::list kids = node.children();

    if(kids.empty()) return;

    const abnf::match& m = kids[0];
    const std::string what = m.name();

    if(what == "raw-text") {
        // layout() reads the bytes on either side of this run, which are not
        // part of it -- hence the whole source rather than just the match.
        std::string t = layout(src, m.begin(), m.end());

        if(!t.empty()) out.push_back(span{ std::move(t), true });

        return;
    }

    if(what == "comment") return;

    if(what == "output") {
        const value v = eval(m.child("expr"), s);
        const text t = v.type() == value::kind::string ? v.str()
                                                       : literal_text(v.flat());

        out.insert(out.end(), t.begin(), t.end());

        return;
    }

    if(what == "set-stmt") {
        s[std::string(m.child("name").text())] = eval(m.child("expr"), s);

        return;
    }

    if(what == "if-block")  { render_if(src, m, s, out);  return; }
    if(what == "for-block") { render_for(src, m, s, out); return; }

    throw tmpl::exception("a node this does not understand: " + what);
}

}

// --------------------------------------------------------------------- tmpl

tmpl::tmpl(std::string source)
    : m_source(std::make_shared<const std::string>(std::move(source)))
{
    const abnf::parse_result r = grammar().at("template").try_parse(*m_source);

    if(!r)
        throw exception(std::string("this template does not parse: ") +
                        r.why().what());

    m_root = r.root();
}

text tmpl::render(const value& context) const
{
    scope s;

    if(context.type() == value::kind::map) {
        const std::map<std::string, value>& f = context.fields();

        for(std::map<std::string, value>::const_iterator i = f.begin();
            i != f.end(); ++i)
            s[i->first] = i->second;
    }

    const abnf::match t = m_root.name() == "template" ? m_root
                                                      : m_root["template"];

    if(!t) throw exception("the parse produced no template");

    const abnf::match::list nodes = direct(t, "node");
    text out;

    for(std::size_t i = 0; i < nodes.size(); i++)
        render_nodes(*m_source, nodes[i], s, out);

    return out;
}

std::string tmpl::str(const value& context) const
{
    return flatten(render(context));
}

}
}
}
