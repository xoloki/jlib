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

// The refresh-token grant, against a token endpoint written here.
//
// Neither Google nor Microsoft can be reached from make check -- both need a
// registered application and a human at a browser -- so what is testable is
// everything up to the moment a real provider is involved: the form that goes
// out, the reply that comes back in every shape a provider actually sends it,
// and what the session does with the answer.
//
// The parts that carry a real risk of being wrong are the ones a test can
// reach: rotation, which locks a user out if the interface cannot hand a new
// refresh token back; the XOAUTH2 message, whose separator is an octet that C++
// will silently mis-escape; and the second challenge, which is how a bad token
// is reported and which hangs a client that answers only the first.

#include "httpserver.hh"

#include <jlib/net/oauth.hh>

#include <jlib/util/URL.hh>
#include <jlib/util/util.hh>

#include <iostream>
#include <string>

namespace oauth = jlib::net::oauth;

using jlib::util::URL;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static std::string show(const std::string& s) {
    std::string out;

    for(unsigned char c : s) {
        if(c == '\001')    out += "^A";
        else if(c == '\r') out += "\\r";
        else if(c == '\n') out += "\\n";
        else if(c < 0x20)  out += "\\" + std::to_string(static_cast<int>(c));
        else               out += static_cast<char>(c);
    }

    return out;
}

/** A client pointed at a test server, with the plaintext guard lifted. */
static oauth::client client_for(const httpserver::server& s) {
    oauth::client c;

    c.token_endpoint = s.url("/token");
    c.id = "jlib-test";
    c.allow_http = true;

    return c;
}

static void a_reply_in_every_shape_a_provider_sends_it() {
    std::cout << "\na reply, in every shape a provider sends it:\n";

    {
        // Google: expires_in as a number, no refresh_token in the reply.
        const oauth::token t = oauth::parse_token_response(
            "{\"access_token\":\"ya29.A\",\"expires_in\":3599,"
            "\"scope\":\"https://mail.google.com/\",\"token_type\":\"Bearer\"}",
            "1//old", 1000);

        ok("an access token reads", t.access() == "ya29.A", t.access());
        ok("the type and scope read", t.type() == "Bearer" &&
           t.scope() == "https://mail.google.com/");
        ok("expires_in becomes a moment", t.expires_at() == 1000 + 3599,
           std::to_string(t.expires_at()));

        // No refresh_token in the reply means the one that was sent is still
        // good -- and it is what the caller must go on storing.
        ok("with no refresh_token, the one that was sent stays",
           t.refresh() == "1//old" && !t.rotated(), t.refresh());
    }

    {
        // Microsoft v1: expires_in as the *string* "3599".  util::json coerces
        // it; before this arc it threw, because it demanded json_type_int.
        const oauth::token t = oauth::parse_token_response(
            "{\"access_token\":\"EwB.A\",\"expires_in\":\"3599\","
            "\"token_type\":\"Bearer\"}", "M.old", 1000);

        ok("expires_in as a string reads too", t.expires_at() == 1000 + 3599,
           std::to_string(t.expires_at()));
    }

    {
        // Microsoft rotates on every refresh and kills the old one at the same
        // moment.  An interface that could not hand this back would log the
        // user out after one refresh and could not be fixed without changing
        // every caller.
        const oauth::token t = oauth::parse_token_response(
            "{\"access_token\":\"EwB.B\",\"refresh_token\":\"M.new\","
            "\"expires_in\":3599}", "M.old", 1000);

        ok("a rotated refresh token comes back", t.refresh() == "M.new", t.refresh());
        ok("and says that it rotated", t.rotated());
    }

    {
        // Sent back unchanged is not rotation, and a caller watching for a
        // save should not be woken for it.
        const oauth::token t = oauth::parse_token_response(
            "{\"access_token\":\"a\",\"refresh_token\":\"same\"}", "same", 1000);

        ok("the same refresh token echoed back is not a rotation", !t.rotated());
    }

    {
        // No expires_in at all is "the endpoint did not say", not "never".
        const oauth::token t = oauth::parse_token_response(
            "{\"access_token\":\"a\"}", "r", 1000);

        ok("no expires_in means no stated expiry", t.expires_at() == 0);
        ok("and such a token is not treated as already dead", !t.expired(1000));
    }

    {
        const oauth::token t = oauth::parse_token_response(
            "{\"access_token\":\"a\",\"expires_in\":3599}", "r", 1000);

        ok("a token is expired before it expires, by a minute of slack",
           !t.expired(1000 + 3538) && t.expired(1000 + 3540),
           "clocks disagree; a token that dies mid-command looks like a wrong "
           "password");
    }
}

static void what_the_endpoint_says_when_it_says_no() {
    std::cout << "\nwhat the endpoint says when it says no:\n";

    // RFC 6749 5.2.  This is the reason the HTTP client hands back a non-2xx
    // instead of throwing: the body is the only thing that says whether to
    // retry or to send the user back to a browser.
    bool caught = false;

    try {
        oauth::parse_token_response(
            "{\"error\":\"invalid_grant\",\"error_description\":\"Token has been "
            "expired or revoked.\"}", "r", 1000);
    }
    catch(oauth::denied& d) {
        caught = true;

        ok("the error code is readable", d.code() == "invalid_grant", d.code());
        ok("and the description", !d.description().empty(), d.description());

        // The one distinction that matters: nothing will fix this but
        // authorizing again, so a client that retries is wasting its time and
        // a client that gives up on a transient error is wrong the other way.
        ok("invalid_grant is fatal", d.fatal());
    }

    ok("an error body throws denied", caught);

    caught = false;

    try {
        oauth::parse_token_response("{\"error\":\"temporarily_unavailable\"}", "r", 1000);
    }
    catch(oauth::denied& d) {
        caught = true;
        ok("and a transient one is not fatal", !d.fatal());
    }

    ok("which is still a refusal", caught);

    // A reply with neither an error nor an access token is not something to
    // guess about.
    caught = false;

    try { oauth::parse_token_response("{\"token_type\":\"Bearer\"}", "r", 1000); }
    catch(oauth::error&) { caught = true; }

    ok("a reply with no access_token is an error", caught);

    caught = false;
    std::string message;

    try { oauth::parse_token_response("<html>go away</html>", "r", 1000); }
    catch(oauth::error& e) { caught = true; message = e.what(); }

    ok("and so is one that is not JSON at all", caught);

    // The body is a bearer token when the request worked and often still one
    // when it did not.  An exception message goes to a log.
    ok("whose message does not quote the body",
       message.find("go away") == std::string::npos, message);
}

static void the_request_that_goes_out() {
    std::cout << "\nthe request that goes out:\n";

    httpserver::server s([](const httpserver::request&) {
        return httpserver::reply(200, "OK",
                                 "{\"access_token\":\"A\",\"expires_in\":3599}",
                                 "Content-Type: application/json\r\n");
    });

    oauth::client c = client_for(s);

    c.secret = "not-a-secret";
    c.scope = "https://mail.google.com/";

    const oauth::token t = oauth::refresh(c, "1//r+e/f=");

    ok("the exchange completes", t.access() == "A", t.access());

    const std::vector<httpserver::request> sent = s.requests();

    ok("one POST was made", sent.size() == 1 &&
       sent[0].head.find("POST /token HTTP/1.1\r\n") == 0,
       std::to_string(sent.size()));

    if(sent.empty()) return;

    ok("with grant_type=refresh_token, per RFC 6749 6",
       sent[0].body.find("grant_type=refresh_token") != std::string::npos,
       sent[0].body);

    // The one that would fail silently: base64 produces "+" and "/" and "=",
    // and this encoding is HTML's, where "+" means a space.  A refresh token
    // sent through uri::encode arrives with spaces in it and is rejected as
    // invalid_grant, which reads exactly like a revoked token.
    ok("and a refresh token whose characters need escaping survives",
       sent[0].body.find("refresh_token=1%2F%2Fr%2Be%2Ff%3D") != std::string::npos,
       sent[0].body);

    ok("the client id and scope go too",
       sent[0].body.find("client_id=jlib-test") != std::string::npos &&
       sent[0].body.find("scope=https%3A%2F%2Fmail.google.com%2F") != std::string::npos);
}

static void a_plaintext_endpoint_is_refused() {
    std::cout << "\na plaintext endpoint is refused:\n";

    // The request carries the refresh token and the client secret, and the
    // access token comes back the same way.
    //
    // Against a server here, not a name on the internet.  The first draft of
    // this pointed at http://example.com/token expecting the connection to
    // fail, and it did not: the request went out and came back 405.  A test
    // that reaches the network is a test that depends on the network, and this
    // one was about to assert something about a stranger's web server.
    httpserver::server s([](const httpserver::request&) {
        return httpserver::reply(200, "OK", "{\"access_token\":\"A\"}");
    });

    oauth::client c = client_for(s);

    c.allow_http = false;
    c.secret = "not-a-secret";

    bool threw = false;
    std::string why;

    try { oauth::refresh(c, "r"); }
    catch(oauth::error& e) { threw = true; why = e.what(); }

    ok("http:// is refused by default", threw);
    ok("and the refusal says what was wrong with it",
       why.find("plaintext") != std::string::npos, why);

    // Refused before anything was sent, which is the assertion that matters:
    // the credential never reached the socket.
    ok("nothing was sent to it", s.requests().empty(),
       std::to_string(s.requests().size()) + " request(s)");

    c.allow_http = true;

    ok("and allowed only when a caller says so",
       oauth::refresh(c, "r").access() == "A");
}

static void a_session_keeps_one_token() {
    std::cout << "\na session keeps one token, and knows when not to:\n";

    int fetches = 0;

    httpserver::server s([&fetches](const httpserver::request&) {
        const std::string n = std::to_string(++fetches);

        return httpserver::reply(200, "OK",
                                 "{\"access_token\":\"A" + n + "\","
                                 "\"refresh_token\":\"R" + n + "\","
                                 "\"expires_in\":3599}");
    });

    std::string stored;
    int stores = 0;

    oauth::session session(client_for(s), "R0",
                           [&stored, &stores](const oauth::token& t) {
                               stored = t.refresh();
                               stores++;
                           });

    ok("the first access fetches one", session.access() == "A1" && fetches == 1);

    // The point of a cache: an IMAP client asks for the token on every
    // connection and a token endpoint has a rate limit.
    ok("the second does not", session.access() == "A1" && fetches == 1,
       std::to_string(fetches) + " fetches");

    // The rotated token is handed to the store callback the instant it
    // arrives, before anything uses the access token beside it -- because a
    // crash in between means the refresh token on disk is the dead one.
    ok("the rotated refresh token reached the store callback",
       stored == "R1" && stores == 1, stored);

    ok("and the session is holding it", session.refresh_token() == "R1",
       session.refresh_token());

    // The cache is an optimisation and never the truth.  Tokens get revoked
    // mid-session and clocks disagree; a NO from IMAP is authoritative whatever
    // the expiry said.
    session.rejected();

    ok("after a rejection it fetches again", session.access() == "A2" && fetches == 2);
    ok("and the second rotation was stored too", stored == "R2" && stores == 2);

    // The second exchange must have sent the rotated token, not the one the
    // session was constructed with -- Microsoft kills the old one on rotation,
    // so sending it again is a lockout.
    const std::vector<httpserver::request> sent = s.requests();

    ok("and the second request sent the rotated token, not the original",
       sent.size() == 2 && sent[1].body.find("refresh_token=R1") != std::string::npos,
       sent.size() >= 2 ? sent[1].body : "");
}

static void the_xoauth2_message() {
    std::cout << "\nthe XOAUTH2 message:\n";

    const std::string m = oauth::xoauth2("joe@example.com", "ya29.TOKEN");

    // Google's and Microsoft's spelling:
    //     user=<email>^Aauth=Bearer <token>^A^A
    ok("it is user, then auth, separated by 0x01",
       m == std::string("user=joe@example.com\001auth=Bearer ya29.TOKEN\001\001"),
       show(m));

    // The trap this exists to avoid.  A hexadecimal escape in C++ has no
    // length limit: "\x01auth=" is not 0x01 followed by "auth=", it is the
    // single character 0x1A followed by "uth=" -- and it compiles silently.
    // The octal form takes three digits at most.
    ok("the separator really is one octet, 0x01",
       m.find('\001') != std::string::npos &&
       m.find('\032') == std::string::npos,
       "\\x01a would have been 0x1A");

    ok("and it ends with two of them",
       m.size() >= 2 && m[m.size() - 1] == '\001' && m[m.size() - 2] == '\001');

    // What actually goes on the wire is this, base64-encoded, which is what
    // Imap4::authenticate_xoauth2 hands to the SASL driver.
    const std::string wire = jlib::util::base64::encode(m);

    ok("and it survives the base64 the SASL driver applies",
       jlib::util::base64::decode(wire) == m);
}

int main() {
    std::cout << std::unitbuf;

    a_reply_in_every_shape_a_provider_sends_it();
    what_the_endpoint_says_when_it_says_no();
    the_request_that_goes_out();
    a_plaintext_endpoint_is_refused();
    a_session_keeps_one_token();
    the_xoauth2_message();

    // What a green run does not establish.
    //
    // That Google or Microsoft accepts any of it.  Neither can be reached from
    // make check: both need a registered application and a human at a browser,
    // so the library can be complete and the feature still not work for a given
    // user until they have registered one.  Every reply parsed here is one this
    // test wrote, in a shape taken from the providers' documentation rather
    // than from a provider.
    //
    // Nor TLS to a token endpoint.  The server here is plaintext with
    // allow_http set, which is exactly what refresh() refuses by default -- and
    // the real thing is an https:// POST to a host with a public certificate
    // chain, which nothing here has made.
    //
    // Nor the authorization-code flow.  There is no way to obtain a first
    // refresh token in jlib yet; this reads mail with one that came from
    // somewhere else.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
