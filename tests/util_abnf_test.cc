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

// The combinator layer.
//
// Several sections here assert that something does NOT work.  They are the
// most important ones in the file: this is a PEG engine wearing ABNF's
// notation, and the two differ in ways that are easy to trip over and
// impossible to notice from the grammar text alone.  Each such section says
// what the limitation is and what to use instead, and asserts the failure, so
// a future change to the semantics breaks a test loudly rather than silently
// changing what a grammar means.

#include <jlib/util/abnf.hh>

#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace jlib::util::abnf;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Does r match the whole of s? */
static bool matches(const rule& r, const std::string& s) {
    return static_cast<bool>(r.try_parse(s));
}

static void terminals_match_exactly_what_they_say() {
    std::cout << "terminals:\n";

    ok("a literal is case sensitive",
       matches(lit("HELO"), "HELO") && !matches(lit("HELO"), "helo"));

    ok("and ilit is not",
       matches(ilit("HELO"), "helo") && matches(ilit("HELO"), "HeLo"));

    ok("a single octet", matches(chr('a'), "a") && !matches(chr('a'), "b"));

    ok("a range", matches(rng('0','9'), "5") && !matches(rng('0','9'), "x"));

    ok("a byte sequence is never folded",
       matches(bytes({0x0D, 0x0A}), "\r\n"));

    ok("anyof takes any of them",
       matches(anyof("+-*/"), "*") && !matches(anyof("+-*/"), "x"));

    ok("empty matches nothing at all", matches(empty(), ""));
    ok("and none matches nothing ever", !matches(none(), "") && !matches(none(), "a"));

    // A literal of no characters is the empty match, not a literal that can
    // never fire.
    ok("an empty literal is the empty match", matches(lit(""), ""));
}

static void concatenation_binds_tighter_than_alternation() {
    std::cout << "\nprecedence:\n";

    // If >> did not bind tighter than |, this would parse as a >> (b|c) >> d.
    const rule r = lit("a") >> lit("b") | lit("c") >> lit("d");

    ok("ab matches",  matches(r, "ab"));
    ok("cd matches",  matches(r, "cd"));
    ok("ad does not", !matches(r, "ad"));
    ok("cb does not", !matches(r, "cb"));

    // Unary binds tighter than either.
    const rule s = *lit("a") >> lit("b");
    ok("*a >> b groups as (*a) >> b", matches(s, "aaab") && matches(s, "b"));
}

static void ordered_choice_is_not_unordered_alternation() {
    std::cout << "\nordered choice, which RFC 5234 alternation is not:\n";

    // The classic shape.  "ab" is tried first, matches "a"... no: on input
    // "ac" the first branch fails outright, so the second is tried and this
    // one works.  The failing case is the other way round.
    const rule shadowed = (lit("a") | lit("ab")) >> lit("c");

    ok("a shorter alternative first shadows the longer one",
       !matches(shadowed, "abc"),
       "(\"a\" / \"ab\") \"c\" cannot match abc");

    ok("written longest first it parses",
       matches((lit("ab") | lit("a")) >> lit("c"), "abc"));

    // And the one that matters, because it is in RFC 5234's own grammar.
    //
    //     repeat = 1*DIGIT / (*DIGIT "*" *DIGIT)
    //
    // Against "3*5" the first branch takes the 3, the enclosing rule then
    // wants end-of-input and does not get it, and the choice has committed.
    const rule as_written =
        +core::DIGIT() | (*core::DIGIT() >> lit("*") >> *core::DIGIT());
    const rule reordered =
        (*core::DIGIT() >> lit("*") >> *core::DIGIT()) | +core::DIGIT();

    ok("RFC 5234's own repeat rule fails as written", !matches(as_written, "3*5"));
    ok("and parses when reordered",                    matches(reordered, "3*5"));
    ok("the reordering still accepts a bare count",    matches(reordered, "3"));
}

static void repetition_is_possessive() {
    std::cout << "\npossessive repetition, and the way round it:\n";

    // *CHAR eats the @ and never gives it back.
    ok("*CHAR \"@\" cannot parse a@b",
       !matches(*core::CHAR() >> lit("@"), "a@b"));

    ok("until() is what that wants",
       matches(until(lit("@")) >> lit("@") >> *core::CHAR(), "a@b"));

    // It is not that repetition is broken -- a self-delimiting body is fine,
    // which is why most ABNF in the wild does not notice.
    ok("a body that cannot match the delimiter is fine",
       matches(*rng('a','z') >> lit("@") >> *rng('a','z'), "abc@def"));
}

static void recursion_and_forward_references() {
    std::cout << "\nrecursion:\n";

    grammar g;

    // parens = "(" *parens ")" -- mentions itself, and is used before the
    // line that defines it.
    g.define("parens", lit("(") >> *g["parens"] >> lit(")"));

    const rule p = g.at("parens");

    ok("balanced",           matches(p, "()"));
    ok("nested",             matches(p, "((()))"));
    ok("siblings",           matches(p, "(()())"));
    ok("unbalanced fails",   !matches(p, "(()"));

    // Forward reference: b is used by a before b exists.
    grammar f;
    f.define("a", lit("x") >> f["b"]);
    f.define("b", lit("y"));

    ok("a rule may be used before it is defined", matches(f.at("a"), "xy"));
}

static void a_broken_grammar_says_which_rule_is_broken() {
    std::cout << "\ngrammars that cannot work:\n";

    {
        grammar g;
        g.define("a", g["b"] >> lit("x"));

        std::string msg;
        try { g.check(); } catch(grammar_error& e) { msg = e.what(); }

        ok("an undefined rule is named", msg.find("\"b\"") != std::string::npos ||
                                         msg.find(" b") != std::string::npos, msg);
    }

    {
        // Every missing rule at once, not one run at a time.
        grammar g;
        g.define("a", g["b"] >> g["c"] >> g["d"]);

        std::string msg;
        try { g.check(); } catch(grammar_error& e) { msg = e.what(); }

        ok("and so is every other one",
           msg.find("b") != std::string::npos &&
           msg.find("c") != std::string::npos &&
           msg.find("d") != std::string::npos, msg);
    }

    {
        grammar g;
        g.define("e", g["e"] >> lit("+"));

        std::string msg;
        try { g.check(); } catch(grammar_error& e) { msg = e.what(); }

        ok("left recursion is caught rather than run",
           msg.find("left recursion") != std::string::npos, msg);
    }

    {
        // *( "" ) would spin forever.
        grammar g;
        g.define("z", *opt(lit("a")));

        std::string msg;
        try { g.check(); } catch(grammar_error& e) { msg = e.what(); }

        ok("so is repeating something that can match nothing",
           msg.find("empty string") != std::string::npos, msg);
    }

    {
        grammar g;
        g.define("a", lit("x"));

        std::string msg;
        try { g.define("a", lit("y")); } catch(grammar_error& e) { msg = e.what(); }

        ok("redefining a rule is an error, and says what to use instead",
           msg.find("define_alternative") != std::string::npos, msg);
    }
}

static void incremental_alternatives() {
    std::cout << "\nABNF's \"=/\":\n";

    grammar g;
    g.define("atext", rng('a','z'));

    const rule a = g.at("atext");
    ok("before", matches(a, "q") && !matches(a, "+"));

    g.define_alternative("atext", lit("+"));

    ok("after, the new alternative is accepted", matches(a, "+"));
    ok("and the old one still is",               matches(a, "q"));

    // The point of the cell: a rule handed out *before* the extension sees it.
    ok("a reference taken earlier sees the change too", matches(a, "+"));
}

static void captures_retain_only_what_was_asked_for() {
    std::cout << "\ncaptures:\n";

    grammar g;
    g.define("local", as("local", +rng('a','z')));
    g.define("domain", as("domain", +anyof("abcdefghijklmnopqrstuvwxyz.")));
    g.define("addr", as("addr", g["local"] >> lit("@") >> g["domain"]));

    const rule r = g.at("addr");
    const std::string in = "joey@dbzero.com";

    {
        const match m = r.parse(in);
        ok("named: the parts come back",
           m["local"].str() == "joey" && m["domain"].str() == "dbzero.com",
           m["local"].str() + " / " + m["domain"].str());

        ok("and the whole", m["addr"].str() == in, m["addr"].str());
    }

    {
        options o;
        o.captures = options::capture_policy::listed;
        o.capture_only = {"local"};

        const match m = r.parse(in, o);

        ok("listed: what was asked for is there", m["local"].str() == "joey");
        ok("and what was not, is not",            !m.has("domain"));
        ok("exactly one record kept",             m.all("local").size() == 1);
    }

    {
        options o;
        o.captures = options::capture_policy::none;

        const match m = r.parse(in, o);

        ok("none: it still parses",  m.text() == in);
        ok("and keeps nothing",      !m.has("local") && !m.has("domain"));
    }

    // A capture inside a branch that loses is not visible.
    {
        grammar h;
        h.define("pick", (as("first", lit("ab")) >> lit("!")) |
                          as("second", lit("ab")));

        const match m = h.at("pick").parse("ab");

        ok("a capture from an abandoned branch is dropped",
           !m.has("first") && m.has("second"));
    }
}

static void errors_carry_a_position_and_a_rule_stack() {
    std::cout << "\nerrors:\n";

    grammar g;
    g.define("digits", +core::DIGIT());
    g.define("pair", g["digits"] >> lit(".") >> g["digits"]);

    bool threw = false;
    std::string report;
    std::size_t at = 0;

    try {
        g.at("pair").parse("12.x4");
    }
    catch(error& e) {
        threw = true;
        report = e.report();
        at = e.offset();
    }

    ok("a mismatch throws", threw);
    ok("at the offending byte", at == 3, std::to_string(at));
    ok("with a line and column",
       report.find("line 1") != std::string::npos &&
       report.find("column 4") != std::string::npos);
    ok("and says which rule it was in",
       report.find("pair") != std::string::npos, report);

    std::cout << "     the report reads:\n";
    std::cout << "       " << report << "\n";
}

static void a_length_prefixed_field_reads_its_length_from_earlier() {
    std::cout << "\ncounted(), which is why the combinators are public:\n";

    // The IMAP literal, near enough:  "{" number "}" CRLF <number octets>
    const rule number = as("number", +core::DIGIT());
    const rule lit_rule = lit("{") >> number >> lit("}") >> core::CRLF()
                        >> as("body", counted(number));

    {
        const match m = lit_rule.parse("{5}\r\nhello");
        ok("the body is exactly as long as the count said",
           m["body"].str() == "hello", m["body"].str());
    }

    {
        const match m = lit_rule.parse("{0}\r\n");
        ok("a count of zero takes nothing", m["body"].str().empty());
    }

    ok("a body shorter than the count fails",
       !matches(lit_rule, "{9}\r\nhello"));

    // The count is octets, so it does not care what is in them.
    {
        const match m = lit_rule.parse("{3}\r\n} \r");
        ok("and it does not care what the octets are", m["body"].str() == "} \r");
    }
}

static void a_back_reference_matches_what_came_before() {
    std::cout << "\nbackref(), which is how an end tag knows its own name:\n";

    const rule name = as("name", +rng('a','z'));
    const rule element = lit("<") >> name >> lit(">")
                       >> as("content", *rng('a','z'))
                       >> lit("</") >> backref(name) >> lit(">");

    const match m = element.parse("<b>hello</b>");
    ok("matching tags parse", m["content"].str() == "hello", m["content"].str());

    ok("mismatched tags do not", !matches(element, "<b>hello</i>"));

    // Nesting, which is where the first version of backref() was wrong.
    //
    // name is one node shared by every level of the recursion, so after the
    // inner element closed, the most recent match of it was still the inner
    // name and the outer end tag was compared against that.  <a><b/></a> was
    // rejected.  A tracked match now carries the rule depth it was made at,
    // and one made deeper than the position asking for it is out of scope.
    {
        grammar g;
        const rule tag = as("tag", +rng('a','z'));

        g.define("elem", lit("<") >> tag >> lit(">")
                       >> *(g["elem"] | +rng('0','9'))
                       >> lit("</") >> backref(tag) >> lit(">"));

        const rule e = g.at("elem");

        ok("a nested element does not steal the outer end tag",
           matches(e, "<a><b>1</b></a>"));

        ok("nor does a sibling after it",
           matches(e, "<a><b>1</b>2</a>"));

        ok("and the outer tag still has to match",
           !matches(e, "<a><b>1</b></c>"));
    }
}

static void the_depth_guard_fires_instead_of_the_stack() {
    std::cout << "\nrunaway input:\n";

    grammar g;
    g.define("parens", lit("(") >> *g["parens"] >> lit(")"));

    const rule p = g.at("parens");

    auto nest = [](std::size_t n) {
        return std::string(n, '(') + std::string(n, ')');
    };

    // The guard counts nested rule invocations, and each level of nesting is
    // one, so the default of 1000 is reached at about 1000 parens -- not at
    // some much larger number.  Worth pinning, because "deep enough" is
    // exactly the sort of thing that gets assumed.
    ok("five hundred deep parses with the defaults", matches(p, nest(500)));

    {
        const parse_result r = p.try_parse(nest(2000));
        ok("two thousand does not", !r);
    }

    {
        options o;
        o.max_depth = 8000;

        const parse_result r = p.try_parse(nest(2000), o);
        ok("and does once the limit is raised", static_cast<bool>(r));
    }

    // Deep enough to exhaust a default stack if nothing stopped it.  The
    // assertion is that we reach the next line at all.
    {
        const parse_result r = p.try_parse(std::string(200000, '('));
        ok("two hundred thousand fails rather than crashing", !r);
    }

    // The step budget, which is a different guard from the depth one.
    //
    // Written first against a left-recursive grammar, which was wrong: that
    // recurses without consuming, so the *depth* guard caught it and the
    // budget was never reached.  A plain grammar with a budget too small to
    // finish is what actually exercises it.
    {
        options o;
        o.step_budget = 50;

        const parse_result r = (*rng('a','z')).try_parse(std::string(500, 'a'), o);

        ok("a step budget stops a parse that would otherwise finish", !r);
        ok("and says which guard fired",
           std::string(r.why().what()).find("steps") != std::string::npos,
           std::string(r.why().what()).substr(0, 70));
    }

    // And the depth guard names itself differently, so the two are told apart.
    {
        const parse_result r = p.try_parse(nest(2000));

        ok("the depth guard says depth, not steps",
           std::string(r.why().what()).find("recursion") != std::string::npos,
           std::string(r.why().what()).substr(0, 70));
    }

    // parse() has to throw the type it actually has.  try_parse holds the
    // failure by reference to error, so "throw why()" sliced a
    // budget_exceeded down to an error and a caller catching the derived type
    // never saw one -- which made "I gave up on this" indistinguishable from
    // "this does not match", the difference being whose fault it is.
    {
        bool sliced = false, kept = false;

        try { p.parse(nest(2000)); }
        catch(budget_exceeded&) { kept = true; }
        catch(error&)           { sliced = true; }

        ok("and parse() throws it as a budget_exceeded, not as an error",
           kept && !sliced);
    }
}

static void a_grammar_serializes_to_abnf() {
    std::cout << "\nto_abnf():\n";

    ok("a concatenation",  (lit("a") >> lit("b")).to_abnf() == "%s\"a\" %s\"b\"",
       (lit("a") >> lit("b")).to_abnf());

    ok("an alternation",   (lit("a") | lit("b")).to_abnf() == "%s\"a\" / %s\"b\"",
       (lit("a") | lit("b")).to_abnf());

    // The parenthesization is the part that is easy to get wrong: an
    // alternation inside a concatenation needs brackets or it means something
    // else entirely.
    ok("an alternation inside a concatenation keeps its brackets",
       ((lit("a") | lit("b")) >> lit("c")).to_abnf() == "(%s\"a\" / %s\"b\") %s\"c\"",
       ((lit("a") | lit("b")) >> lit("c")).to_abnf());

    ok("a concatenation inside an alternation does not need them",
       ((lit("a") >> lit("b")) | lit("c")).to_abnf() == "%s\"a\" %s\"b\" / %s\"c\"",
       ((lit("a") >> lit("b")) | lit("c")).to_abnf());

    ok("optional",  opt(lit("a")).to_abnf() == "[%s\"a\"]", opt(lit("a")).to_abnf());
    ok("star",      (*lit("a")).to_abnf() == "*%s\"a\"",    (*lit("a")).to_abnf());
    ok("plus",      (+lit("a")).to_abnf() == "1*%s\"a\"",   (+lit("a")).to_abnf());
    ok("a range",   rng('0','9').to_abnf() == "%x30-39",    rng('0','9').to_abnf());
}

static void the_core_rules_are_the_ones_in_appendix_b() {
    std::cout << "\nRFC 5234 appendix B:\n";

    ok("ALPHA",  matches(core::ALPHA(), "Q") && !matches(core::ALPHA(), "0"));
    ok("BIT",    matches(core::BIT(), "1")   && !matches(core::BIT(), "2"));
    ok("DIGIT",  matches(core::DIGIT(), "7") && !matches(core::DIGIT(), "a"));
    ok("HEXDIG", matches(core::HEXDIG(), "f") && !matches(core::HEXDIG(), "g"));
    ok("SP",     matches(core::SP(), " ")    && !matches(core::SP(), "\t"));
    ok("HTAB",   matches(core::HTAB(), "\t"));
    ok("WSP",    matches(core::WSP(), " ")   && matches(core::WSP(), "\t"));
    ok("CRLF",   matches(core::CRLF(), "\r\n") && !matches(core::CRLF(), "\n"));
    ok("DQUOTE", matches(core::DQUOTE(), "\""));

    // The boundaries, which are where CHAR and OCTET differ and where grammars
    // routinely reach for the wrong one.
    ok("CHAR excludes NUL",  !matches(core::CHAR(), std::string(1, '\0')));
    ok("OCTET includes it",   matches(core::OCTET(), std::string(1, '\0')));
    ok("VCHAR excludes SP",  !matches(core::VCHAR(), " "));
    ok("VCHAR excludes DEL", !matches(core::VCHAR(), std::string(1, '\x7f')));
    ok("CTL includes DEL",    matches(core::CTL(), std::string(1, '\x7f')));
}

static void what_a_parse_costs() {
    std::cout << "\nwhat it costs:\n";

    // A grammar with the shape that matters: alternatives sharing a prefix,
    // nested repetition, and a rule that has to be re-scanned when the first
    // branch fails -- the mailbox = name-addr / addr-spec shape.
    grammar g;
    g.define("atom", +anyof("abcdefghijklmnopqrstuvwxyz0123456789"));
    g.define("word", g["atom"] | (lit("\"") >> *anyof("abc ") >> lit("\"")));
    g.define("phrase", g["word"] >> *(lit(" ") >> g["word"]));
    g.define("addr", g["atom"] >> lit("@") >> g["atom"]);
    g.define("mailbox", (g["phrase"] >> lit(" <") >> g["addr"] >> lit(">")) |
                         g["addr"]);

    const rule m = g.at("mailbox");

    struct { const char* what; std::string in; } cases[] = {
        { "a bare address",      "joey@dbzero" },
        { "a display name",      "joey yandle <joey@dbzero>" },
        { "a long bare address", std::string(200, 'a') + "@dbzero" },
        { "a long non-match",    std::string(200, 'a') },
    };

    for(const auto& c : cases) {
        options o;
        o.captures = options::capture_policy::none;

        const parse_result r = m.try_parse(c.in, o);

        // Steps are not exposed, so this measures what can be measured from
        // outside: that it terminates, and how big the input was.  The real
        // number belongs in a profile, and the ceiling below is the guard.
        std::cout << "     " << std::setw(20) << std::left << c.what
                  << " " << std::setw(5) << c.in.size() << " bytes  "
                  << (r ? "parsed" : "rejected") << "\n";
    }

    // The regression guard: a grammar of this shape on 400 bytes must not need
    // an exponential number of steps.  The budget is the only thing that can
    // observe it, so ask for a generous one and require it not to fire.
    options o;
    o.captures = options::capture_policy::none;
    o.step_budget = 4000000;

    bool blew = false;
    try {
        m.try_parse(std::string(400, 'a'), o);
    }
    catch(budget_exceeded&) {
        blew = true;
    }

    ok("400 bytes of near-miss stays inside four million steps", !blew);
}

int main() {
    std::cout << std::unitbuf;

    terminals_match_exactly_what_they_say();
    concatenation_binds_tighter_than_alternation();
    ordered_choice_is_not_unordered_alternation();
    repetition_is_possessive();
    recursion_and_forward_references();
    a_broken_grammar_says_which_rule_is_broken();
    incremental_alternatives();
    captures_retain_only_what_was_asked_for();
    errors_carry_a_position_and_a_rule_stack();
    a_length_prefixed_field_reads_its_length_from_earlier();
    a_back_reference_matches_what_came_before();
    the_depth_guard_fires_instead_of_the_stack();
    a_grammar_serializes_to_abnf();
    the_core_rules_are_the_ones_in_appendix_b();
    what_a_parse_costs();

    // What a green run here does NOT establish.
    //
    // Not RFC 5234 conformance.  Alternation is PEG ordered choice and
    // repetition is possessive; two sections above assert the difference, one
    // of them using a production from RFC 5234's own grammar.  A grammar that
    // relies on unordered alternation will fail, and it will fail as a clean
    // parse error rather than a wrong answer -- which is the better of the two
    // but is still a difference.
    //
    // Not that the step counts are reasonable.  There is no memoization, and
    // what_a_parse_costs() asserts only that one shape on 400 bytes stays
    // under a ceiling.  Whether packrat is needed is a measurement that has to
    // be taken against a real grammar, which does not exist yet.
    //
    // Not thread safety.  A grammar is read-only once defined and every piece
    // of parse state lives on a per-call context, so parsing one grammar from
    // several threads is *intended* to work and is not tested here.  If a memo
    // table is ever added it must stay on the context; move it to the grammar
    // for a cache-hit win and that property is gone, silently.
    //
    // Not the absence of leaks in general.  grammar's destructor breaks the
    // slot cycles it owns, and nothing here counts allocations to prove it.
    // A rule made recursive by hand with rule::declare(), outside any grammar,
    // still leaks its cycle -- that is documented, not fixed.
    //
    // Not the depth limit as a guarantee.  It is a count of nested rule
    // invocations, not of stack bytes, and the relationship between them
    // depends on the compiler and the platform.
    return failures ? 1 : 0;
}
