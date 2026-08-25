/* -*- mode: C++ c-basic-offset: 4 -*-
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
 */

// The Date: header, read against RFC 5322 section 3.3.
//
// Live: Email::operator< parses both sides every time a mailbox is sorted by
// date.  What was there guessed a strftime format token by token, and could
// not fail -- is_timezone() returned true for every possible input, so the two
// "couldn't parse" throws below it were unreachable and any string at all
// produced a format that strptime then half-applied.

#include <jlib/util/Date.hh>
#include <jlib/util/rfc5322.hh>

#include <jlib/util/abnf.hh>

#include <iostream>
#include <string>

using jlib::util::Date;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static time_t when(const char* s) {
    try { return Date::parse_rfc5322(s); }
    catch(Date::exception&) { return -1; }
}

static void the_canonical_example() {
    std::cout << "RFC 5322 appendix A.1.1:\n";

    // The date on the RFC's own example message.
    ok("Fri, 21 Nov 1997 09:55:06 -0600",
       when("Fri, 21 Nov 1997 09:55:06 -0600") == 880127706,
       std::to_string(when("Fri, 21 Nov 1997 09:55:06 -0600")));

    // The instant is absolute: the zone in the header is applied, so this does
    // not depend on where the machine reading it is.  The old code reached for
    // strptime's %Z, which cannot set an offset at all.
    ok("the same instant, written five ways",
       when("Fri, 21 Nov 1997 15:55:06 +0000") == 880127706 &&
       when("Fri, 21 Nov 1997 15:55:06 GMT")   == 880127706 &&
       when("Fri, 21 Nov 1997 15:55:06 UT")    == 880127706 &&
       when("Fri, 21 Nov 1997 10:55:06 EST")   == 880127706 &&
       when("Fri, 21 Nov 1997 07:55:06 PST")   == 880127706);

    // The day-name is not consulted.  RFC 5322 3.3 says it may disagree with
    // the date, and the date wins.
    ok("a wrong day-name does not change the answer",
       when("Sun, 21 Nov 1997 09:55:06 -0600") == 880127706);
}

static void real_headers() {
    std::cout << "\nwhat arrives in the post:\n";

    // Every one of these is the same instant written differently.
    struct { const char* what; const char* s; } same[] = {
        { "no day-name",          "21 Nov 1997 09:55:06 -0600" },
        { "no space after comma", "Fri,21 Nov 1997 09:55:06 -0600" },
        { "a trailing comment",   "Fri, 21 Nov 1997 09:55:06 -0600 (CST)" },
        { "a comment mid-date",   "Fri, 21 (the 21st) Nov 1997 09:55:06 -0600" },
        { "folded before the zone",
          "Fri, 21 Nov 1997 09:55:06\r\n  -0600" },
        { "leading and trailing space",
          "  Fri, 21 Nov 1997 09:55:06 -0600  " },
    };

    for(const auto& g : same) {
        ok(g.what, when(g.s) == 880127706, std::to_string(when(g.s)));
    }

    // These are different instants, so they get their own assertions -- the
    // first draft of this test put them in the list above and expected them to
    // come out the same, which they very much should not.
    ok("no seconds means zero seconds",
       when("Fri, 21 Nov 1997 09:55 -0600") == 880127706 - 6,
       std::to_string(when("Fri, 21 Nov 1997 09:55 -0600")));

    ok("a one-digit day is that day",
       when("Sat, 1 Nov 1997 09:55:06 -0600") ==
       when("Sat, 01 Nov 1997 09:55:06 -0600"),
       std::to_string(when("Sat, 1 Nov 1997 09:55:06 -0600")));

    ok("and it is twenty days before the twenty-first",
       880127706 - when("Sat, 1 Nov 1997 09:55:06 -0600") == 20 * 86400);
}

static void obsolete_syntax() {
    std::cout << "\nsection 4.3, which real mail is full of:\n";

    // 4.3: two digits, 00-49 is 20xx and 50-99 is 19xx; three digits are the
    // year minus 1900.  The old code decided between %y and %Y by whether the
    // token was four characters long and left the rest to strptime.
    ok("a two-digit year in the nineties",
       when("Fri, 21 Nov 97 09:55:06 -0600") == 880127706,
       std::to_string(when("Fri, 21 Nov 97 09:55:06 -0600")));

    ok("and one in the twenty-first century",
       when("Sat, 01 Jan 05 00:00:00 +0000") == when("Sat, 01 Jan 2005 00:00:00 +0000"));

    ok("00 is 2000, not 1900",
       when("Sat, 01 Jan 00 00:00:00 +0000") == when("Sat, 01 Jan 2000 00:00:00 +0000"));
    ok("50 is 1950",
       when("Sun, 01 Jan 50 00:00:00 +0000") == when("Sun, 01 Jan 1950 00:00:00 +0000"));
    ok("and three digits are 1900 plus",
       when("Thu, 01 Jan 104 00:00:00 +0000") == when("Thu, 01 Jan 2004 00:00:00 +0000"));

    // Comments anywhere a field may carry CFWS.  This is the reason the digits
    // have rules of their own: obs-day's span can be " 21 (the 21st) " and a
    // comment is allowed to contain digits, so scraping the span would read
    // the comment as part of the date.
    ok("a comment with a number in it is not the number",
       when("Fri, 21 (the 2nd) Nov 1997 09:55:06 -0600") == 880127706,
       std::to_string(when("Fri, 21 (the 2nd) Nov 1997 09:55:06 -0600")));
}

static void zones() {
    std::cout << "\nzones:\n";

    const time_t noon = when("Thu, 01 Jan 1970 12:00:00 +0000");

    ok("+0000 is the epoch's noon", noon == 43200, std::to_string(noon));
    ok("a positive offset is subtracted",
       when("Thu, 01 Jan 1970 12:00:00 +0100") == noon - 3600);
    ok("and a negative one added",
       when("Thu, 01 Jan 1970 12:00:00 -0100") == noon + 3600);
    ok("minutes count too",
       when("Thu, 01 Jan 1970 12:00:00 +0530") == noon - (5 * 3600 + 30 * 60));

    ok("the named North American zones",
       when("Thu, 01 Jan 1970 12:00:00 EST") == noon + 5 * 3600 &&
       when("Thu, 01 Jan 1970 12:00:00 EDT") == noon + 4 * 3600 &&
       when("Thu, 01 Jan 1970 12:00:00 PST") == noon + 8 * 3600);

    // Not RFC 5322, and everywhere.  Without it "UT" matches and strands the
    // "C", so the whole header fails.
    ok("UTC, which the RFC does not list", when("Thu, 01 Jan 1970 12:00:00 UTC") == noon);

    // RFC 5322 4.3: the single military letters "were defined incorrectly" in
    // RFC 822, and a parser SHOULD treat them as -0000.  Reading "A" as +0100
    // would be repeating the older document's mistake.
    ok("a military letter parses and means unknown",
       when("Thu, 01 Jan 1970 12:00:00 A") == noon &&
       when("Thu, 01 Jan 1970 12:00:00 Z") == noon);

    // -0000 means "the zone is not known", which is not the same as UTC, but
    // there is nowhere in a time_t to put the difference.
    ok("-0000 reads as zero", when("Thu, 01 Jan 1970 12:00:00 -0000") == noon);
}

static void it_can_fail_now() {
    std::cout << "\nwhat it refuses:\n";

    // The whole of #93.  is_timezone() was:
    //
    //     // i used to actually give a damn about this field, for now
    //     // i'm just going to call anything I don't recognize a timezone
    //     return true;
    //
    // and it was the last test in both branches of the classifier, so nothing
    // ever reached a throw and every one of these produced a format string.
    for(const char* s : { "garbage", "", "   ", "not a date at all",
                          "Mon, 25 Aug 2026",            // no time
                          "25 Aug 2026 14:30:00",        // no zone
                          "Mon, 32 Foo 2026 25:99 -0700",// no such month
                          "Mon, 25 Aug 2026 14:30:00 -07", // short zone
                          "Mon, 25 Aug 2026 1:2:3 -0700" }) {
        ok(std::string("\"") + s + "\"", !Date::valid(s));
    }

    // And it says where.
    std::string msg;

    try { Date::parse_rfc5322("Mon, 32 Foo 2026 25:99 -0700"); }
    catch(Date::exception& e) { msg = e.what(); }

    ok("with a column", msg.find("column") != std::string::npos, msg);

    // set() throws too, which is the entry point Email uses.
    bool threw = false;

    try { Date d; d.set("garbage"); }
    catch(Date::exception&) { threw = true; }

    ok("and set() does not silently produce a wrong time", threw);
}

static void the_grammar_itself() {
    std::cout << "\nthe grammar:\n";

    using namespace jlib::util::abnf;

    bool built = false;
    std::string why;

    try {
        grammar g = compile(std::string(jlib::util::rfc5322::LEXICAL) +
                            jlib::util::rfc5322::DATE_TIME);
        g.check();
        built = true;
    }
    catch(exception& e) { why = e.what(); }

    ok("it compiles and checks", built, why);

    // Every month name, since a wrong table here is a silent twelve-month
    // error rather than a parse failure.
    static const char* const MONTHS[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    time_t previous = 0;
    bool ordered = true;

    for(int i = 0; i < 12; i++) {
        const std::string s = std::string("01 ") + MONTHS[i] + " 2001 00:00:00 +0000";
        const time_t t = when(s.c_str());

        if(t <= previous) ordered = false;

        previous = t;
    }

    ok("the twelve months are in order and a month apart", ordered);

    ok("January the first, 2001, is where it should be",
       when("Mon, 01 Jan 2001 00:00:00 +0000") == 978307200,
       std::to_string(when("Mon, 01 Jan 2001 00:00:00 +0000")));

    // A leap day, which the civil-days arithmetic has to get right: 2000 was
    // a leap year, 1900 was not, and the rule that decides is the one people
    // implement wrong.
    ok("29 February 2000 is where it should be",
       when("Tue, 29 Feb 2000 00:00:00 +0000") == 951782400,
       std::to_string(when("Tue, 29 Feb 2000 00:00:00 +0000")));

    ok("and 1 March 1900 is one day after 28 February 1900",
       when("Thu, 01 Mar 1900 00:00:00 +0000") -
       when("Wed, 28 Feb 1900 00:00:00 +0000") == 86400);

    // Shape, not sense.  "30 Feb" parses and rolls over, which is what the
    // disclaimer at the bottom is about.
    ok("30 February parses, because the grammar checks shape",
       Date::valid("Wed, 30 Feb 2000 00:00:00 +0000"));
}

int main() {
    std::cout << std::unitbuf;

    the_canonical_example();
    real_headers();
    obsolete_syntax();
    zones();
    it_can_fail_now();
    the_grammar_itself();

    // What a green run does NOT establish.
    //
    // Not that the date is real.  The grammar checks shape, not sense:
    // "31 Feb", "25:99:99" and a day of 99 all parse, and the arithmetic
    // rolls them over into the following month or day rather than refusing
    // them.  RFC 5322 does not require a parser to reject them and mail
    // contains them; a client that dropped a message because its clock was
    // wrong would be worse than one that filed it a day late.
    //
    // Not the local-time accessors.  parse_rfc5322 returns an absolute
    // instant; Date::set(time_t) then runs it through localtime, so year(),
    // hour() and the rest depend on TZ.  Nothing here sets TZ, so those are
    // not asserted -- what is asserted is the time_t, which does not.
    //
    // Not -0000.  RFC 5322 3.3 distinguishes it from +0000: it means the zone
    // is unknown rather than that the sender was at UTC.  There is nowhere in
    // a time_t to keep that, so both read as zero.
    //
    // Not the other direction.  Date::get() still builds its output with the
    // strftime-style code this branch did not touch.
    return failures ? 1 : 0;
}
