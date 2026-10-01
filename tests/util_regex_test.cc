/* -*- mode: C++ c-basic-offset: 4 -*- */

// util::Regex, which had no test at all.
//
// It is a thin wrapper over POSIX <regex.h>, kept because a library user may
// reasonably want one -- but jlib itself no longer has a caller: the two
// that existed were both parsing things the ABNF grammars describe, and both
// were wrong about them.  So this is the only thing checking it.
//
// The value-semantics cases are the point.  Match owns two raw arrays and
// Regex owns a compiled regex_t, and all of the copying is hand-written.

#include <jlib/util/Regex.hh>

#include <iostream>
#include <string>
#include <utility>

using jlib::util::Regex;

static int failures = 0;

static void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << "\n";

    if(!ok)
        failures++;
}

int main() {
    // Matching, and that group 0 is the whole match.
    {
        Regex r("abc");
        Regex::Match m = r("xxabcxx");

        check(bool(m), "a pattern present in the subject matches");
        check(m[0] == "abc", "group 0 is the matched text, got \"" + m[0] + "\"");
    }

    // Subgroups.
    {
        Regex r("([0-9]+)-([0-9]+)");
        Regex::Match m = r("id 12-34 end");

        check(m[0] == "12-34", "group 0 spans the whole match");
        check(m[1] == "12",    "group 1");
        check(m[2] == "34",    "group 2");
    }

    // A subject that does not match yields a false Match, and indexing it is
    // defined rather than a crash.
    {
        Regex r("zzz");
        Regex::Match m = r("abc");

        check(!bool(m), "a pattern absent from the subject does not match");
        check(m[0] == "", "indexing a failed match gives an empty string");
    }

    // Out of range, and an optional group that did not participate.  POSIX
    // reports the latter as rm_so == -1, which must not become a substring
    // call with a negative offset.
    {
        Regex r("(a)(b)?");
        Regex::Match m = r("a");

        check(m[1] == "a", "a group that participated");
        check(m[2] == "", "a group that did not participate is empty");
        check(m[99] == "", "an out-of-range group is empty, not a crash");
    }

    // Flags reach regcomp.
    {
        Regex r("ABC", REG_EXTENDED | REG_ICASE);

        check(bool(r("xxabcxx")), "REG_ICASE is honoured");
    }

    // A pattern regcomp rejects must throw rather than leave a half-built
    // object that is used later.
    {
        bool threw = false;

        try { Regex r("("); }
        catch(Regex::exception&) { threw = true; }

        check(threw, "an unbalanced pattern throws");
    }

    // Regex::operator[] reads the last match -- the call sites relied on it.
    {
        Regex r("([0-9]+)-([0-9]+)");

        r("12-34");

        check(r[1] == "12", "operator[] reads the most recent match");

        r("56-78");

        check(r[1] == "56", "and is updated by the next one");
    }

    // Value semantics.  Both types hand-roll their copying.
    {
        Regex a("([0-9]+)");
        Regex b(a);

        check(b("x42")[1] == "42", "a copy-constructed Regex matches");

        Regex c("zzz");
        c = a;

        check(c("x42")[1] == "42", "an assigned Regex matches");
    }

    // Self-assignment.  Match::copy calls destroy() before reading its
    // source, so `m = m` frees the arrays and then reads them.
    {
        Regex r("([0-9]+)");
        Regex::Match m = r("x42");

        m = m;

        check(m[1] == "42", "a Match survives self-assignment");

        r = r;

        check(r("x7")[1] == "7", "a Regex survives self-assignment");
    }

    // Repeated assignment, which is where a missing regfree shows up as
    // growth rather than as a wrong answer.  Correctness only here; the leak
    // is argued at the fix.
    {
        Regex r("a");

        for(int i = 0; i < 100; i++)
            r = Regex("([0-9]+)");

        check(r("x9")[1] == "9", "a Regex reassigned many times still matches");
    }

    // Group counting comes from regcomp's re_nsub, not from counting '('
    // in the pattern.  Counting them claimed groups that do not exist.
    {
        Regex plain("abc");

        check(plain("abc").size() == 1, "a pattern with no groups has size 1");

        Regex bracketed("[(]x[)]");

        check(bracketed("(x)").size() == 1,
              "parens inside a bracket expression are not groups, size "
              + std::to_string(bracketed("(x)").size()));

        Regex escaped("a\\(b");

        check(escaped("a(b").size() == 1,
              "an escaped paren is not a group, size "
              + std::to_string(escaped("a(b").size()));

        Regex two("(a)(b)");

        check(two("ab").size() == 3, "two groups plus the whole match");
    }

    // Match owns a string and a vector now, so moving works and leaves the
    // source usable.  It could not move at all before.
    {
        Regex r("([0-9]+)");
        Regex::Match m = r("x42");
        Regex::Match moved = std::move(m);

        check(moved[1] == "42", "a moved-from Match hands over its result");
    }

    std::cout << (failures ? "FAILED" : "PASSED") << "\n";

    return failures ? 1 : 0;
}
