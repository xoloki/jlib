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

// URLs, read against RFC 3986's own grammar.
//
// This is where every mail account in gtkmail comes from -- "imaps://user:pass
// @mail.example.com/INBOX" in a config file -- so getting the pieces wrong
// means connecting to the wrong host, or with the wrong password, or not at
// all.  It used to be five POSIX regexes applied to each other's output.

#include <jlib/util/URL.hh>
#include <jlib/util/rfc3986.hh>

#include <jlib/util/abnf.hh>

#include <iostream>
#include <string>

using jlib::util::URL;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static void a_mail_account() {
    std::cout << "what a mail account looks like:\n";

    const URL u("imaps://joe:secret@mail.example.com:993/INBOX");

    ok("scheme", u.get_protocol() == "imaps", u.get_protocol());
    ok("user",   u.get_user() == "joe", u.get_user());
    ok("pass",   u.get_pass() == "secret", u.get_pass());
    ok("host",   u.get_host() == "mail.example.com", u.get_host());
    ok("port",   u.get_port() == "993" && u.get_port_val() == 993, u.get_port());
    ok("path",   u.get_path() == "/INBOX", u.get_path());

    const URL bare("imap://mail.example.com/INBOX");

    ok("and without credentials",
       bare.get_user().empty() && bare.get_pass().empty() &&
       bare.get_host() == "mail.example.com" && bare.get_port().empty());

    // A local mbox: a scheme, an empty authority, and an absolute path.
    const URL local("mbox:///home/joe/Mail/inbox");

    ok("a local mailbox has no host and a path",
       local.get_host().empty() && local.get_path() == "/home/joe/Mail/inbox",
       local.get_path());
}

static void the_trim_that_was_dead() {
    std::cout << "\nthe trim:\n";

    // parse() computed `const std::string text = trim(url)` and then matched
    // every one of its five anchored regexes against `url`, untrimmed.  So a
    // config file line with a trailing newline on it did not parse.
    const URL u("  imaps://mail.example.com/INBOX\n");

    ok("leading and trailing whitespace is ignored",
       u.get_protocol() == "imaps" && u.get_path() == "/INBOX",
       u.coagulate());
}

static void the_cascade_split_in_the_wrong_places() {
    std::cout << "\nwhere the old regexes went wrong:\n";

    // FULL_URL required user AND pass AND port AND path AND query all present,
    // so it never matched an ordinary URL and everything fell through to a
    // cascade of [[:graph:]] patterns applied in a fixed order.

    // HOST_WITH_USER was ^([[:graph:]]+)@([[:graph:]]+)$, and "@" is a graph
    // character, so leftmost-longest took "a@b" as the user.  There is no
    // reading under which that is a URL: neither userinfo nor host may
    // contain an unescaped "@".
    ok("two at-signs is not a URL", !URL::valid("http://a@b@c/"));

    // HOST_WITH_PATH ran first, so a password containing "/" was cut into the
    // path and the rest of the authority went with it.  "/" is not allowed in
    // userinfo, so this is not a URL either -- and saying so beats silently
    // connecting somewhere else.
    ok("a slash in the userinfo is not a URL",
       !URL::valid("imap://user:pa/ss@host/INBOX"));

    // Percent-encode it and it is fine, which is the answer the RFC gives.
    const URL u("imap://user:pa%2Fss@host/INBOX");

    ok("percent-encoded, it parses and decodes",
       u.get_pass() == "pa/ss" && u.get_host() == "host", u.get_pass());

    // An email address as a username is how several providers do it.
    const URL at("imap://joe%40example.com:pw@mail.example.com/INBOX");

    ok("an at-sign in a username, encoded",
       at.get_user() == "joe@example.com" && at.get_host() == "mail.example.com",
       at.get_user() + " / " + at.get_host());

    // USER_WITH_PASS split on ":" with the same greedy pattern.  RFC 3986
    // allows a colon in the userinfo and everyone splits at the first.
    const URL colon("imap://user:a:b@host/");

    ok("a colon in the password splits at the first one",
       colon.get_user() == "user" && colon.get_pass() == "a:b",
       colon.get_user() + " / " + colon.get_pass());
}

static void case_folding() {
    std::cout << "\nRFC 3986 6.2.2.1:\n";

    const URL u("IMAPS://Mail.Example.COM/INBOX");

    // Every caller in jlib was doing lower(get_protocol()) at the point of
    // comparison; doing it once, here, is where it belongs.
    ok("the scheme folds", u.get_protocol() == "imaps", u.get_protocol());
    ok("and the host",     u.get_host() == "mail.example.com", u.get_host());

    // The path does not.  RFC 3986 is explicit that only the scheme and the
    // host are case insensitive, and an IMAP mailbox name is case sensitive.
    ok("the path does not", u.get_path() == "/INBOX", u.get_path());
}

static void ip_literals() {
    std::cout << "\nIPv6 literals:\n";

    const URL u("http://[::1]:8080/x");

    // Without the brackets: they delimit the literal and getaddrinfo does not
    // want them.
    ok("come back without their brackets", u.get_host() == "::1", u.get_host());
    ok("with the port beside them",        u.get_port() == "8080");
    ok("and go back in with them",
       u.coagulate() == "http://[::1]:8080/x", u.coagulate());

    for(const char* s : { "http://[2001:db8::1]/", "http://[::ffff:1.2.3.4]/",
                          "http://[2001:0db8:0000:0000:0000:ff00:0042:8329]/" }) {
        ok(s, URL::valid(s));
    }

    // The "::" is what defeats a faithful transcription of RFC 3986's
    // IPv6address: "*5( h16 \":\" )" eats the first of the two colons and
    // possessive repetition does not give it back.  See rfc3986.hh.
    ok("an address with :: in the middle parses",
       URL("http://[2001:db8::1]/").get_host() == "2001:db8::1");

    ok("and empty brackets do not", !URL::valid("http://[]/"));
    ok("nor brackets full of text", !URL::valid("http://[not an addr]/"));

    // A dotted quad is a perfectly good registered name as far as the grammar
    // is concerned, which is why IPv4address is not in the host rule.
    ok("a dotted quad parses",     URL("http://1.2.3.4/").get_host() == "1.2.3.4");
    ok("and so does one with five parts",
       URL("http://1.2.3.4.5/").get_host() == "1.2.3.4.5",
       URL("http://1.2.3.4.5/").get_host());
}

static void query_strings() {
    std::cout << "\nquery strings:\n";

    const URL u("http://host/p?x=1&y=hello%20world&flag&z=a%3Db");

    ok("the raw query is kept as written",
       u.get_qs() == "x=1&y=hello%20world&flag&z=a%3Db", u.get_qs());

    // parse_qs never decoded, so a value written "%20" came back as three
    // characters.
    ok("and the pairs are decoded",
       u["x"] == "1" && u["y"] == "hello world", u["y"]);

    ok("a name with no value is a name, not a dropped token",
       u.get_qs_hash().count("flag") == 1 && u["flag"].empty());

    ok("and an encoded equals stays in the value",
       u["z"] == "a=b", u["z"]);

    // Round trip through the map form, which now encodes.
    std::map<std::string,std::string> m;

    m["a b"] = "c&d";

    URL v("http://host/");
    v.set_qs(m);

    ok("building a query escapes what has to be escaped",
       v.get_qs() == "a%20b=c%26d", v.get_qs());
    ok("and reading it back gives the same pairs",
       URL(v.coagulate())["a b"] == "c&d", URL(v.coagulate())["a b"]);

    ok("a fragment is not part of the query",
       URL("http://host/p?a=1#top").get_fragment() == "top" &&
       URL("http://host/p?a=1#top").get_qs() == "a=1");
}

static void round_trip() {
    std::cout << "\nround trip:\n";

    for(const char* s : {
        "imaps://joe:secret@mail.example.com:993/INBOX",
        "imap://mail.example.com/INBOX",
        "mbox:///home/joe/Mail/inbox",
        "http://[::1]:8080/x",
        "http://host/a/b?x=1&y=2",
        "http://host/",
        "mailto:joe@example.com",
    }) {
        ok(s, URL(s).coagulate() == s, URL(s).coagulate());
    }

    // What parse() decoded, coagulate() re-encodes -- otherwise a password
    // with an "@" in it produces a URL that reads back with a different host,
    // which is a wrong connection rather than an error.
    const URL u("imap://joe%40example.com:p%40ss@host/INBOX");

    ok("credentials are re-encoded on the way out",
       u.coagulate() == "imap://joe%40example.com:p%40ss@host/INBOX",
       u.coagulate());
    ok("and survive a second parse",
       URL(u.coagulate()).get_pass() == "p@ss");
}

static void what_it_refuses() {
    std::cout << "\nnot a URL:\n";

    for(const char* s : { "no-scheme", "", "://x", "1http://x/",
                          "http://user:pa ss@host/", "http://host/\tx" }) {
        ok(std::string("\"") + s + "\"", !URL::valid(s));
    }

    // "http:/" *is* a URI -- a scheme and an absolute path with nothing in it.
    // It was on the list above until the test was run: RFC 3986 does not
    // require an authority, and refusing this would be inventing a rule.
    ok("\"http:/\" is a URL, with no authority", URL::valid("http:/"));

    // A relative reference is a URI-reference and not a URI.  jlib has no base
    // to resolve one against, so refusing it beats guessing a scheme.
    ok("\"example.com/x\" is a relative reference",
       !URL::valid("example.com/x"));

    // And the error says where.
    std::string msg;

    try { URL u("http://a@b@c/"); }
    catch(URL::exception& e) { msg = e.what(); }

    ok("the message carries a column", msg.find("column") != std::string::npos,
       msg.substr(0, 40));
}

static void the_grammar_itself() {
    std::cout << "\nthe grammar:\n";

    using namespace jlib::util::abnf;

    bool built = false;
    std::string why;

    try {
        grammar g = compile(jlib::util::rfc3986::URI_GRAMMAR);
        g.check();
        built = true;
    }
    catch(exception& e) { why = e.what(); }

    ok("it compiles and checks", built, why);

    // dec-octet is the other production reordered for ordered choice: as the
    // RFC writes it, the bare DIGIT is first and takes the "2" of "255".
    grammar g = compile(jlib::util::rfc3986::URI_GRAMMAR);
    options o;
    o.captures = options::capture_policy::none;

    for(const char* s : { "0", "9", "10", "99", "100", "199", "249", "250", "255" }) {
        ok(std::string("dec-octet accepts ") + s,
           static_cast<bool>(g.at("dec-octet").try_parse(s, o)));
    }

    for(const char* s : { "256", "300", "1000", "01" }) {
        ok(std::string("and refuses ") + s,
           !g.at("dec-octet").try_parse(s, o));
    }
}

int main() {
    std::cout << std::unitbuf;

    a_mail_account();
    the_trim_that_was_dead();
    the_cascade_split_in_the_wrong_places();
    case_folding();
    ip_literals();
    query_strings();
    round_trip();
    what_it_refuses();
    the_grammar_itself();

    // What a green run does NOT establish.
    //
    // Not RFC 3986 conformance.  IPv6address is restructured rather than
    // transcribed -- the RFC's nine counted alternatives cannot work under
    // possessive repetition -- and the version here accepts things the RFC
    // does not: more than eight groups, an IPv4address somewhere other than
    // the end.  rfc3986.hh says exactly what and why.  Everything else is
    // Appendix A verbatim but for dec-octet's order.
    //
    // Not normalisation.  RFC 3986 section 6 has an algorithm for deciding
    // whether two URIs are equivalent -- removing dot segments, normalising
    // percent-encoding, adding a default port.  None of it is here, so
    // "http://x/a/../b" and "http://x/b" are different URLs to this class.
    //
    // Not scheme-specific rules.  Whether a scheme has an authority, what its
    // default port is, whether userinfo means anything: RFC 3986 leaves all of
    // that to the scheme and so does this.  "imap://-b.example/" parses.
    //
    // Not IDN.  A host is octets.  RFC 3987 and punycode are not implemented,
    // so an internationalised hostname has to arrive already encoded.
    //
    // Not the zone identifier.  "fe80::1%25eth0" is RFC 6874, which RFC 3986
    // predates, and it does not parse.
    return failures ? 1 : 0;
}
