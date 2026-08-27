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

// The authorization-code flow with PKCE: obtaining the first refresh token.
//
// Three things here are worth more than the rest.  RFC 7636 Appendix B
// publishes a verifier and the challenge it must produce, so the PKCE
// arithmetic is checked against the RFC rather than against itself.  The state
// check is the client's only defence against someone else's authorization code
// being planted on it, so it is tested by planting one.  And the whole flow
// runs -- a thread standing in for the browser, jlib's own HTTP client fetching
// the redirect, and a token endpoint that verifies the PKCE binding by hashing
// the verifier it was sent and comparing it to the challenge it saw earlier.

#include "httpserver.hh"

#include <jlib/net/http.hh>
#include <jlib/net/oauth.hh>

#include <jlib/sys/socketstream.hh>

#include <jlib/util/URL.hh>
#include <jlib/util/util.hh>

#include <iostream>
#include <string>
#include <thread>

namespace oauth = jlib::net::oauth;
namespace http = jlib::net::http;

using jlib::util::URL;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static void base64url_is_not_base64() {
    std::cout << "\nbase64url is not base64:\n";

    // The three characters that differ, chosen so all of them appear.
    const std::string raw("\xfb\xff\xbe", 3);

    ok("\"+\" becomes \"-\" and \"/\" becomes \"_\"",
       jlib::util::base64::encode(raw) == "+/++" &&
       jlib::util::base64url::encode(raw) == "-_--",
       jlib::util::base64url::encode(raw));

    // RFC 7636 4.2 requires the padding off.  A provider compares the whole
    // string, so a challenge with "=" on the end does not match -- and the
    // error it returns says "invalid_request", which explains nothing.
    ok("and the padding comes off",
       jlib::util::base64url::encode("a") == "YQ" &&
       jlib::util::base64::encode("a") == "YQ==",
       jlib::util::base64url::encode("a"));

    // Lengths, so every remainder mod 3 is covered, and one with a NUL and a
    // high byte in it.  Written with explicit lengths: as a const char* the
    // binary case stops at the NUL and silently becomes the empty string,
    // which is a test that passes without testing anything.
    const std::string cases[] = {
        std::string(""), std::string("a"), std::string("ab"),
        std::string("abc"), std::string("abcd"),
        std::string("\x00\x01\xfe\xff", 4),
        std::string("any old bytes at all")
    };

    for(const std::string& in : cases) {
        ok("round trip: " + std::to_string(in.length()) + " octets",
           jlib::util::base64url::decode(jlib::util::base64url::encode(in)) == in);
    }

    // Other people's encoders emit padding.  Refusing it would be pedantry.
    ok("padding on input is tolerated",
       jlib::util::base64url::decode("YQ==") == "a" &&
       jlib::util::base64url::decode("YQ") == "a");
}

static void pkce_against_the_rfc() {
    std::cout << "\nPKCE, against RFC 7636's own vector:\n";

    // Appendix B.  The one place in this branch where the arithmetic can be
    // checked against the document instead of against itself.
    const std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    const std::string expected = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";

    const oauth::pkce p(verifier);

    ok("the challenge is base64url(SHA-256(verifier)), unpadded",
       p.challenge() == expected, p.challenge());

    ok("and the verifier is kept as it was", p.verifier() == verifier);

    // RFC 7636 4.2 also defines "plain", where the challenge is the verifier.
    // RFC 8252 8.1 says a native application must use S256; a client that can
    // be talked down to plain has no protection at all.
    ok("the method is S256 and nothing else",
       std::string(oauth::pkce::method()) == "S256");

    const oauth::pkce fresh = oauth::pkce::generate();

    // 32 random octets is 43 base64url characters, which is the minimum RFC
    // 7636 4.1 allows.
    ok("a generated verifier is the length the RFC requires",
       fresh.verifier().length() == 43, std::to_string(fresh.verifier().length()));

    ok("and two of them differ", oauth::pkce::generate().verifier() !=
       oauth::pkce::generate().verifier());

    // Checked rather than assumed, because a caller supplying its own is where
    // it can be wrong -- and a provider's complaint about it is
    // "invalid_request", which says nothing.
    for(const char* bad : { "short", "has spaces in it and is quite long enough otherwise",
                            "has+plus+signs+which+are+not+unreserved+and+is+long+enough" }) {
        bool threw = false;

        try { oauth::pkce p2(bad); }
        catch(oauth::error&) { threw = true; }

        ok(std::string("a verifier that is not RFC 7636 4.1's shape is refused: \"") +
           std::string(bad).substr(0, 12) + "...\"", threw);
    }
}

static void the_url_the_browser_gets() {
    std::cout << "\nthe URL the browser is sent to:\n";

    oauth::client c;

    c.authorize_endpoint = "https://accounts.example.com/o/oauth2/v2/auth";
    c.token_endpoint = "https://oauth2.example.com/token";
    c.id = "jlib.apps.example.com";
    c.scope = "https://mail.example.com/ offline_access";

    const oauth::pkce p("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");

    const std::string url = oauth::authorize_url(c, "http://127.0.0.1:9999/",
                                                 "st-at-e", p);

    URL parsed(url);

    ok("it is the endpoint that was configured",
       parsed.get_host() == "accounts.example.com" &&
       parsed.get_path() == "/o/oauth2/v2/auth", url);

    ok("response_type is code", parsed["response_type"] == "code");
    ok("the client id is there", parsed["client_id"] == c.id);
    ok("the redirect URI is there", parsed["redirect_uri"] == "http://127.0.0.1:9999/",
       parsed["redirect_uri"]);
    ok("the state is there", parsed["state"] == "st-at-e");
    ok("the challenge is there, and not the verifier",
       parsed["code_challenge"] == p.challenge() &&
       url.find(p.verifier()) == std::string::npos);
    ok("and the method is named", parsed["code_challenge_method"] == "S256");

    // A space in a scope is a separator between scopes, and this encoding
    // spells it "+".  Sent as "%20" some providers accept it and some do not.
    ok("the scope's spaces are encoded",
       url.find("scope=https%3A%2F%2Fmail.example.com%2F+offline_access") !=
       std::string::npos, url);

    // Not because a secret goes on this URL -- nothing here does -- but
    // because whoever can rewrite it chooses where the user types a password.
    c.authorize_endpoint = "http://accounts.example.com/auth";

    bool threw = false;

    try { oauth::authorize_url(c, "http://127.0.0.1:9999/", "s", p); }
    catch(oauth::error&) { threw = true; }

    ok("a plaintext authorization endpoint is refused", threw);

    // RFC 6749 10.12 makes state the client's only defence here.
    c.authorize_endpoint = "https://accounts.example.com/auth";
    threw = false;

    try { oauth::authorize_url(c, "http://127.0.0.1:9999/", "", p); }
    catch(oauth::error&) { threw = true; }

    ok("and so is a request with no state", threw);
}

/** Pretend to be a browser: GET the redirect URI with these parameters. */
static void visit(const std::string& uri, const std::string& query) {
    try {
        jlib::util::URL u(uri);

        u.set_qs(query);

        http::get(u);
    }
    catch(std::exception&) {
        // The receiver answers and closes; a client-side error here is not
        // what any of these sections is asserting.
    }
}

static void the_receiver() {
    std::cout << "\nthe redirect receiver:\n";

    {
        oauth::redirect_receiver r;

        ok("it binds loopback and says where",
           r.redirect_uri() == "http://127.0.0.1:" + std::to_string(r.port()) + "/",
           r.redirect_uri());

        // The address literal, not "localhost".  RFC 8252 7.3: "localhost" is
        // whatever the resolver says it is.
        ok("by its address, not by a name",
           r.redirect_uri().find("127.0.0.1") != std::string::npos);

        std::thread browser([&r] {
            visit(r.redirect_uri(), "code=THE-CODE&state=the-state");
        });

        const oauth::callback back = r.wait("the-state", 10);

        browser.join();

        ok("a code arrives", back.code == "THE-CODE", back.code);
        ok("with its state", back.state == "the-state");
        ok("and it is ok()", back.ok());
    }

    {
        // The attack the state exists for.  Somebody else's authorization code
        // planted on this client, which would then exchange it and attach the
        // attacker's account to the user's session.  RFC 6749 10.12.
        oauth::redirect_receiver r;

        std::thread browser([&r] {
            visit(r.redirect_uri(), "code=SOMEONE-ELSES&state=wrong");
        });

        bool threw = false;

        try { r.wait("the-state", 10); }
        catch(oauth::error&) { threw = true; }

        browser.join();

        ok("a callback whose state does not match is refused", threw);
    }

    {
        // A user who clicked "no".  RFC 6749 4.1.2.1 -- an error rather than a
        // code, and it arrives at the same URI.
        oauth::redirect_receiver r;

        std::thread browser([&r] {
            visit(r.redirect_uri(),
                  "error=access_denied&error_description=The+user+said+no&state=s");
        });

        bool denied_thrown = false;
        std::string code;

        try { r.wait("s", 10); }
        catch(oauth::denied& d) { denied_thrown = true; code = d.code(); }
        catch(oauth::error&) {}

        browser.join();

        ok("a refusal comes back as denied, with its reason", denied_thrown &&
           code == "access_denied", code);
    }

    {
        // Whatever connects to a loopback port is not necessarily a browser.
        oauth::redirect_receiver r;

        std::thread other([&r] {
            try {
                jlib::sys::socketstream s("127.0.0.1", r.port());

                s << "PING\r\n\r\n" << std::flush;
            }
            catch(std::exception&) {}
        });

        bool threw = false;

        try { r.wait("s", 10); }
        catch(oauth::error&) { threw = true; }

        other.join();

        ok("something that is not a request line is refused", threw);
    }

    {
        oauth::redirect_receiver r;

        bool threw = false;

        // Nothing connects.  The user closed the browser, or never saw it.
        try { r.wait("s", 0.5); }
        catch(oauth::error&) { threw = true; }

        ok("and waiting for a browser that never comes gives up", threw);
    }
}

static void the_whole_flow() {
    std::cout << "\nthe whole flow, end to end:\n";

    std::string seen_challenge;
    std::string seen_verifier;
    std::string seen_redirect;

    // A token endpoint that checks the PKCE binding for real: it hashes the
    // verifier it was sent and compares it to the challenge it saw in the
    // authorization request.  That is what makes a stolen authorization code
    // useless, so a test that did not check it would be testing the shape of
    // the flow and not the point of it.
    httpserver::server endpoint([&](const httpserver::request& r) {
        const std::map<std::string, std::string> form =
            URL::parse_qs(r.body);

        std::map<std::string, std::string>::const_iterator v = form.find("code_verifier");

        if(form.find("grant_type")->second != "authorization_code")
            return httpserver::reply(400, "Bad Request",
                                     "{\"error\":\"unsupported_grant_type\"}");

        if(v == form.end())
            return httpserver::reply(400, "Bad Request",
                                     "{\"error\":\"invalid_request\"}");

        seen_verifier = v->second;

        std::string derived;

        try { derived = oauth::pkce(v->second).challenge(); }
        catch(std::exception&) {}

        if(derived != seen_challenge)
            return httpserver::reply(400, "Bad Request",
                                     "{\"error\":\"invalid_grant\"}");

        if(form.find("redirect_uri")->second != seen_redirect)
            return httpserver::reply(400, "Bad Request",
                                     "{\"error\":\"invalid_grant\"}");

        return httpserver::reply(200, "OK",
                                 "{\"access_token\":\"AT\",\"refresh_token\":\"RT\","
                                 "\"expires_in\":3600}",
                                 "Content-Type: application/json\r\n");
    });

    oauth::client c;

    c.authorize_endpoint = "http://127.0.0.1:1/authorize";
    c.token_endpoint = endpoint.url("/token");
    c.id = "jlib-test";
    c.allow_http = true;

    std::thread browser;

    try {
        const oauth::token t = oauth::authorize(
            c,
            [&](const std::string& url) {
                // Standing in for a browser.  It reads the authorization URL
                // the way a provider would, and redirects back with a code.
                URL u(url);

                seen_challenge = u["code_challenge"];
                seen_redirect = u["redirect_uri"];

                const std::string state = u["state"];

                browser = std::thread([u, state] {
                    visit(u["redirect_uri"], "code=AUTH-CODE&state=" + state);
                });
            },
            0, 10);

        if(browser.joinable()) browser.join();

        ok("a refresh token comes out the far end", t.refresh() == "RT", t.refresh());
        ok("and an access token with it", t.access() == "AT", t.access());

        // The two halves of PKCE really were connected: the challenge that went
        // in the URL is the hash of the verifier that went to the token
        // endpoint, and the endpoint above rejected anything else.
        ok("the verifier hashes to the challenge that was sent",
           !seen_verifier.empty() && !seen_challenge.empty() &&
           oauth::pkce(seen_verifier).challenge() == seen_challenge);

        ok("the challenge was sent, and the verifier was not, in the URL",
           !seen_challenge.empty() && seen_challenge != seen_verifier);

        // The redirect URI is not a place anything was sent -- it is a value
        // the provider compares, and it has to be the same in both requests.
        ok("the same redirect URI was used in both halves",
           seen_redirect.find("http://127.0.0.1:") == 0, seen_redirect);
    }
    catch(std::exception& e) {
        if(browser.joinable()) browser.join();

        ok("a refresh token comes out the far end", false, e.what());
    }
}

int main() {
    std::cout << std::unitbuf;

    base64url_is_not_base64();
    pkce_against_the_rfc();
    the_url_the_browser_gets();
    the_receiver();
    the_whole_flow();

    // What a green run does not establish.
    //
    // That Google or Microsoft accepts any of it, and this is the branch where
    // that gap is widest: the flow's other half is a provider's consent screen
    // and a human, and neither is reachable from make check.  Worse, **neither
    // provider will issue a client id at all without the user registering an
    // application first** -- so this can be complete and correct and still not
    // work for somebody who has not done that.  The "browser" here is a thread
    // that reads the URL and redirects back, which is the happy path and
    // nothing else.
    //
    // Nor TLS on either endpoint.  Both are plaintext loopback with allow_http
    // set, which is what the code refuses by default.
    //
    // Nor what a real browser does with the page the receiver writes.  It is
    // served, and no test looks at it rendered.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
