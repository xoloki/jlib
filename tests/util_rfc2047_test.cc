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

// RFC 2047 encoded words: how a Subject or a display name carries a character
// that a header field may not.
//
// The address and MIME tests both disclaimed this and said it was Headers'
// job.  This is that job, and the interesting half of it is not the grammar --
// it is section 5, which says where an encoded word may appear, and section
// 6.2, which says what happens to the whitespace between two of them.

#include <jlib/util/encoded_word.hh>
#include <jlib/util/rfc2047.hh>
#include <jlib/util/Headers.hh>

#include <jlib/util/abnf.hh>

#include <iostream>
#include <string>
#include <vector>

using jlib::util::Headers;
namespace rfc2047 = jlib::util::rfc2047;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

// Written out as escapes so the file stays ASCII and the bytes are unambiguous.
static const char* const SCHOENEN = "Sch\xc3\xb6nen Tag";        // Schönen Tag
static const char* const JOSE     = "Jos\xc3\xa9 N\xc3\xba\xc3\xb1" "ez";

static void the_two_encodings() {
    std::cout << "decoding:\n";

    ok("B, which is base64",
       rfc2047::decode("=?utf-8?B?U2Now7ZuZW4gVGFn?=") == SCHOENEN,
       rfc2047::decode("=?utf-8?B?U2Now7ZuZW4gVGFn?="));

    // RFC 2047 4.2: like quoted-printable, except "_" is a space -- because a
    // real space would end the word and "=20" everywhere is unreadable.
    ok("Q, which is quoted-printable with a twist",
       rfc2047::decode("=?utf-8?Q?Sch=C3=B6nen_Tag?=") == SCHOENEN,
       rfc2047::decode("=?utf-8?Q?Sch=C3=B6nen_Tag?="));

    ok("lowercase b and q too",
       rfc2047::decode("=?utf-8?b?U2No?=") == "Sch" &&
       rfc2047::decode("=?utf-8?q?S=63h?=") == "Sch");

    ok("a word among ordinary text",
       rfc2047::decode("Re: =?utf-8?B?U2Now7ZuZW4=?= Tag") == "Re: " + std::string(SCHOENEN),
       rfc2047::decode("Re: =?utf-8?B?U2Now7ZuZW4=?= Tag"));

    ok("and text with no words in it is untouched",
       rfc2047::decode("no encoded words here") == "no encoded words here");

    // RFC 2231 5's language tag, which is legal and rare.
    ok("a language tag is accepted and discarded",
       rfc2047::decode("=?utf-8*de?B?U2No?=") == "Sch",
       rfc2047::decode("=?utf-8*de?B?U2No?="));
}

static void the_charsets_come_back() {
    std::cout << "\nwhich charset:\n";

    std::vector<std::string> cs;

    rfc2047::decode("=?iso-8859-1?Q?a?= =?utf-8?B?Yg==?=", cs);

    ok("one per word, in order",
       cs.size() == 2 && cs[0] == "iso-8859-1" && cs[1] == "utf-8",
       cs.empty() ? "none" : cs[0] + " " + (cs.size() > 1 ? cs[1] : ""));

    // A single out-parameter cannot describe that string, which is a property
    // of RFC 2047 rather than of this code: the two halves are in different
    // encodings and nothing here transcodes.
    std::string one;
    Headers::decode("=?iso-8859-1?Q?a?= =?utf-8?B?Yg==?=", one);

    ok("and Headers::decode reports the first", one == "iso-8859-1", one);

    rfc2047::decode("nothing encoded", cs);
    ok("none when there were no words", cs.empty());

    ok("names fold to lower case",
       (rfc2047::decode("=?UTF-8?B?U2No?=", cs), cs.size() == 1 && cs[0] == "utf-8"),
       cs.empty() ? "" : cs[0]);
}

static void where_a_word_may_appear() {
    std::cout << "\nsection 5, which the grammar cannot say:\n";

    // An encoded word has to be delimited.  Without this rule, a "=?" in the
    // middle of an ordinary token starts a decode -- and a URL with a query
    // string in it is the case that turns up.
    ok("not decoded in the middle of a token",
       rfc2047::decode("x=?utf-8?B?U2No?=") == "x=?utf-8?B?U2No?=");

    ok("nor is a stray one in a URL",
       rfc2047::decode("http://x/?a==?b") == "http://x/?a==?b");

    ok("but it is at the start of the field",
       rfc2047::decode("=?utf-8?B?U2No?= x") == "Sch x");

    ok("and inside a comment",
       rfc2047::decode("(=?utf-8?B?U2No?=)") == "(Sch)",
       rfc2047::decode("(=?utf-8?B?U2No?=)"));

    // Left alone rather than guessed at.
    ok("an unterminated word",
       rfc2047::decode("=?utf-8?B?U2No") == "=?utf-8?B?U2No");
    ok("an encoding that is neither B nor Q",
       rfc2047::decode("=?utf-8?Z?U2No?=") == "=?utf-8?Z?U2No?=");
    ok("a word with a piece missing",
       rfc2047::decode("=?utf-8?B?=") == "=?utf-8?B?=");
    ok("and an empty charset",
       rfc2047::decode("=??B?U2No?=") == "=??B?U2No?=");
}

static void the_whitespace_rule() {
    std::cout << "\nsection 6.2, the whitespace between two words:\n";

    // "the 'linear-white-space' between adjacent 'encoded-word's is ignored"
    // -- because a long name gets folded across two of them and must come back
    // as one word.
    ok("goes away between two adjacent words",
       rfc2047::decode("=?utf-8?B?U2No?= =?utf-8?B?w7ZuZW4=?=") == "Sch\xc3\xb6nen",
       rfc2047::decode("=?utf-8?B?U2No?= =?utf-8?B?w7ZuZW4=?="));

    ok("including a fold",
       rfc2047::decode("=?utf-8?B?U2No?=\r\n =?utf-8?B?w7ZuZW4=?=") == "Sch\xc3\xb6nen");

    // But only between two of them.
    ok("and stays where a word is not on both sides",
       rfc2047::decode("=?utf-8?B?U2No?=  plain  =?utf-8?B?w7ZuZW4=?=")
           == "Sch  plain  \xc3\xb6nen",
       rfc2047::decode("=?utf-8?B?U2No?=  plain  =?utf-8?B?w7ZuZW4=?="));

    ok("including before the first",
       rfc2047::decode("  =?utf-8?B?U2No?=") == "  Sch");
    ok("and after the last",
       rfc2047::decode("=?utf-8?B?U2No?=  ") == "Sch  ");
}

static void encoding() {
    std::cout << "\nencoding:\n";

    ok("ASCII is left exactly alone",
       rfc2047::encode("plain ascii", "utf-8") == "plain ascii");

    // The old encoder started the encoded word at the first byte over 0x7F, so
    // a word with an accent in the middle of it produced
    // "Jose N=?utf-8?B?...?=" -- an encoded word with no whitespace before it,
    // which section 5 does not allow and no conformant reader will decode.
    const std::string one = rfc2047::encode(SCHOENEN, "utf-8");

    ok("a whole word is encoded, not the tail of one",
       one == "=?utf-8?B?U2Now7ZuZW4=?= Tag", one);

    ok("and the ASCII word beside it is not", one.find(" Tag") != std::string::npos);

    // Two adjacent words that both need encoding go into one word, because
    // 6.2 would eat the space between two.  This is the assertion that caught
    // it: the first version emitted two and delivered "JoseNunez".
    const std::string two = rfc2047::encode(JOSE, "utf-8");

    ok("two adjacent words become one, gap included",
       two == "=?utf-8?B?Sm9zw6kgTsO6w7Fleg==?=", two);

    for(const char* s : { "plain ascii", SCHOENEN, JOSE, "", " ", "a  b",
                          "\xc3\xa9", "  leading", "trailing  ",
                          "mixed \xc3\xa9 and ascii \xc3\xa9 again" }) {
        ok(std::string("round trips: \"") + s + "\"",
           rfc2047::decode(rfc2047::encode(s, "utf-8")) == s,
           rfc2047::decode(rfc2047::encode(s, "utf-8")));
    }
}

static void the_length_limit() {
    std::cout << "\nsection 2, seventy-five characters:\n";

    std::string long_word = "a ";

    for(int i = 0; i < 40; i++) long_word += "\xc3\xa9";

    long_word += " b";

    const std::string out = rfc2047::encode(long_word, "utf-8");

    bool fits = true;
    std::size_t words = 0;

    for(std::string::size_type i = 0; (i = out.find("=?", i)) != out.npos; ) {
        const std::string::size_type j = out.find("?=", i + 2);

        if(j == out.npos) break;

        words++;
        if(j + 2 - i > 75) fits = false;
        i = j + 2;
    }

    ok("a long run is split across several words", words > 1,
       std::to_string(words));
    ok("and none of them exceeds 75 characters", fits);
    ok("and it still round trips",
       rfc2047::decode(out) == long_word, rfc2047::decode(out));

    // A charset name long enough to leave no room is still handled rather
    // than producing an encoded word with no text in it.
    const std::string wide =
        rfc2047::encode("\xc3\xa9\xc3\xa9", std::string(80, 'x'));

    ok("an absurd charset name does not produce an empty word",
       wide.find("?B??=") == std::string::npos, wide.substr(0, 40));
}

static void through_headers() {
    std::cout << "\nthrough Headers:\n";

    Headers h("Subject: =?utf-8?B?U2Now7ZuZW4=?= Tag\r\n"
              "From: =?utf-8?Q?Jos=C3=A9?= <jose@x.com>\r\n"
              "\r\n");

    ok("a Subject is decoded",
       h.get("SUBJECT") == SCHOENEN, h.get("SUBJECT"));
    ok("and a display name",
       h.get("FROM") == "Jos\xc3\xa9 <jose@x.com>", h.get("FROM"));

    std::string charset;
    h.get("SUBJECT", charset);

    ok("with the charset it named", charset == "utf-8", charset);

    // A header folded across two encoded words, which is what a long name
    // arrives as and what 6.2 exists for.
    Headers folded("Subject: =?utf-8?B?U2No?=\r\n =?utf-8?B?w7ZuZW4=?=\r\n\r\n");

    ok("a folded pair of words rejoins with no gap",
       folded.get("SUBJECT") == "Sch\xc3\xb6nen", folded.get("SUBJECT"));

    ok("and encode is the inverse",
       Headers::decode(Headers::encode(SCHOENEN, "utf-8"), charset) == SCHOENEN);
}

static void the_grammar_itself() {
    std::cout << "\nthe grammar:\n";

    using namespace jlib::util::abnf;

    bool built = false;
    std::string why;

    try {
        grammar g = compile(jlib::util::rfc2047::ENCODED_WORD);
        g.check();
        built = true;
    }
    catch(exception& e) { why = e.what(); }

    ok("it compiles and checks", built, why);

    // etchar is the one place the RFC states an exclusion in prose and the
    // grammar states ranges, so a reader cannot check it by comparing.  Every
    // character of especials, plus the space, spelled out.  Note it is not
    // RFC 2045's tspecials: especials adds "." and does not exclude "\".
    for(char c : std::string(" ()<>@,;:\"/[]?.=")) {
        const std::string s = std::string("=?ut") + c + "f8?B?U2No?=";

        ok(std::string("etchar excludes '") + c + "'",
           rfc2047::decode(s) == s, rfc2047::decode(s));
    }

    ok("but not a backslash, which especials does not list",
       rfc2047::decode("=?ut\\f8?B?U2No?=") == "Sch",
       rfc2047::decode("=?ut\\f8?B?U2No?="));
}

int main() {
    std::cout << std::unitbuf;

    the_two_encodings();
    the_charsets_come_back();
    where_a_word_may_appear();
    the_whitespace_rule();
    encoding();
    the_length_limit();
    through_headers();
    the_grammar_itself();

    // What a green run does NOT establish.
    //
    // Not that the decoded bytes are readable.  decode() returns the octets
    // the sender encoded, in whatever charset they named; nothing here
    // transcodes, so a header mixing two charsets comes back as a string that
    // no single charset describes.  That is RFC 2047's shape, not this
    // implementation's, and the two-argument form exists so a caller can at
    // least see it happen.
    //
    // Not the full placement rules of section 5.  It gives three contexts --
    // unstructured text, a comment, and a word in a phrase -- with slightly
    // different rules in each, and what is implemented is the one thing common
    // to all three: an encoded word must be delimited.  In particular nothing
    // here refuses an encoded word inside a quoted string, which section 5
    // forbids.
    //
    // Not the 75-character limit on the way in.  It is a MUST that real
    // senders break constantly, so a longer word is accepted; encode() obeys
    // it going out, which is the direction jlib controls.
    //
    // Not Q on the way out.  encode() always writes B, which is correct but
    // wasteful for a string that is mostly ASCII -- "=?utf-8?Q?Sch=C3=B6nen?="
    // is shorter and readable, and a real mail client would choose.
    return failures ? 1 : 0;
}
