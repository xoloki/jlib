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

// jlib::net::Imap4 against a real IMAP server.
//
// Everything else in the suite feeds a std::istringstream, which can be made
// to produce any response at all -- but only a server decides *when* to send
// a literal, and the literal is what the line-based reader could not do.  So
// this one starts a Dovecot on a high port with a Maildir it seeds, and talks
// to it.
//
// Exit 77 (SKIP) when there is no dovecot to start, which is every machine
// that is not the build container.

#include "httpserver.hh"
#include "mailserver.hh"

#include <jlib/net/Imap4.hh>
#include <jlib/net/imap_response.hh>

#include <jlib/sys/socketstream.hh>
#include <jlib/sys/sys.hh>

#include <jlib/net/oauth.hh>

#include <jlib/util/URL.hh>
#include <jlib/util/util.hh>

#include <iostream>
#include <memory>
#include <string>

using mailserver::PASSWORD;
using mailserver::impersonating_body;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static std::string show(const std::string& s) {
    std::string out;

    for(char c : s) {
        if(c == '\r')      out += "\\r";
        else if(c == '\n') out += "\\n";
        else               out += c;
    }

    return out;
}
int main() {
    std::cout << std::unitbuf;

    {
        std::string out, err;

        try {
            if(jlib::sys::run({ "dovecot", "--version" }, out, err) != 0) {
                std::cerr << "dovecot does not run here; skipping\n";
                return 77;
            }
        }
        catch(std::exception& e) {
            std::cerr << "no dovecot: " << e.what() << "; skipping\n";
            return 77;
        }
    }

    mailserver::server s;

    // An HTTP server in this process, wearing two hats at once.  To jlib it is
    // an OAuth2 token endpoint -- POST a refresh token, get an access token.
    // To dovecot it is the tokeninfo endpoint its oauth2 passdb consults to
    // find out whether that access token is any good and whose it is.
    //
    // Which is what makes an end-to-end XOAUTH2 test possible without Google
    // or Microsoft.  Both of them are exactly this shape and neither can be
    // reached from make check.
    httpserver::server tokens([](const httpserver::request& r) {
        if(r.head.find("POST /token") == 0) {
            // A refresh token this test wrote, exchanged for an access token
            // this test will recognise.
            if(r.body.find("refresh_token=good-refresh") == std::string::npos)
                return httpserver::reply(400, "Bad Request",
                                         "{\"error\":\"invalid_grant\"}");

            return httpserver::reply(200, "OK",
                                     "{\"access_token\":\"live-access-token\","
                                     "\"refresh_token\":\"rotated-refresh\","
                                     "\"expires_in\":3600}",
                                     "Content-Type: application/json\r\n");
        }

        // dovecot's half.  Any token but the good one is not joe's.
        if(r.head.find("GET /tokeninfo?access_token=live-access-token ") == 0)
            return httpserver::reply(200, "OK", "{\"email\":\"joe\"}",
                                     "Content-Type: application/json\r\n");

        return httpserver::reply(401, "Unauthorized",
                                 "{\"error\":\"invalid_token\"}",
                                 "Content-Type: application/json\r\n");
    });

    s.tokeninfo_port = tokens.port();

    if(!s.start()) {
        std::cerr << s.log();
        std::cerr << "could not start a server; skipping\n";

        return 77;
    }

    std::cout << "dovecot on 127.0.0.1:" << s.port << "\n";

    jlib::util::URL url("imap://joe@127.0.0.1:" + std::to_string(s.port) + "/INBOX");

    url.set_pass(PASSWORD);

    jlib::net::Imap4 imap(url);

    try {
        std::unique_ptr<jlib::sys::socketstream> sock(imap.connect());

        ok("the greeting reads", true);

        // The whole of (b): every command used to build its arguments by
        // concatenation, so this password produced "LOGIN joe pa ss "quoted"
        // back\slash" and the server answered BAD.
        imap.login(*sock, "", "");

        ok("LOGIN with a space, a quote and a backslash in the password", true);

        std::vector<jlib::net::ListItem> ls = imap.list(*sock, "", "*");

        bool inbox = false;

        for(jlib::net::ListItem& i : ls) {
            if(jlib::util::upper(i.get_name()) == "INBOX") inbox = true;
        }

        ok("LIST finds INBOX", inbox, std::to_string(ls.size()) + " mailboxes");

        // A mailbox whose name needs quoting, which is the same fix seen from
        // the other end.
        imap.create(*sock, "My Mail");
        imap.subscribe(*sock, "My Mail");

        bool spaced = false;

        for(jlib::net::ListItem& i : imap.list(*sock, "", "*")) {
            if(i.get_name().find("My Mail") != std::string::npos) spaced = true;
        }

        ok("a mailbox with a space in its name can be created and listed", spaced);

        // LSUB sent LIST, so it answered with every mailbox rather than the
        // subscribed ones.  INBOX is not subscribed here and "My Mail" is.
        std::vector<jlib::net::ListItem> subs = imap.lsub(*sock, "", "*");

        bool only_subscribed = !subs.empty();

        for(jlib::net::ListItem& i : subs) {
            if(jlib::util::upper(i.get_name()) == "INBOX") only_subscribed = false;
        }

        ok("LSUB answers with the subscribed mailboxes, not all of them",
           only_subscribed, std::to_string(subs.size()) + " subscribed");

        imap.select(*sock, "INBOX");

        ok("SELECT", true);

        // parse() read the SELECT responses with icontains() and an index into
        // a tokenized line.  The UNSEEN branch took tok[2], which on
        // "* OK [UNSEEN 1] First unseen." is the string "[UNSEEN" -- so the
        // count was zero for as long as there was one.
        ok("EXISTS comes back", imap.exists() == 2,
           std::to_string(imap.exists()));
        ok("and UNSEEN is not zero", imap.unseen() == 1,
           std::to_string(imap.unseen()));

        // The reason this test exists.  One of these two messages is a body
        // that says "A00001 OK FETCH completed" and nineteen variations, which
        // is what the client is waiting for -- and it has to come back whole.
        std::string one, two;

        for(int i = 0; i < 2; i++) {
            const std::string raw = imap.get(*sock, i, false).raw();

            if(raw.find("Subject: one") != std::string::npos)      one = raw;
            else if(raw.find("Subject: two") != std::string::npos) two = raw;
        }

        ok("a plain message comes back", one == "From: a@b.c\r\nSubject: one\r\n\r\nhello\r\n",
           show(one));

        ok("and one whose body impersonates every tag jlib uses",
           two == impersonating_body(),
           two == impersonating_body()
               ? std::string()
               : "got " + std::to_string(two.size()) + " of "
                 + std::to_string(impersonating_body().size()) + " octets");

        // SEARCH, which was a bare tag and no command.
        const std::vector<unsigned long> all = imap.search(*sock, "ALL");

        ok("SEARCH ALL finds both messages",
           all.size() == 2 && all[0] == 1 && all[1] == 2,
           std::to_string(all.size()) + " found");

        const std::vector<unsigned long> subject =
            imap.search(*sock, "HEADER SUBJECT \"two\"");

        ok("and a header search finds the one",
           subject.size() == 1 && subject[0] == 2,
           std::to_string(subject.size()) + " found");

        ok("a search that matches nothing returns nothing",
           imap.search(*sock, "HEADER SUBJECT \"nothing here\"").empty());

        // UID, which was also a bare tag.  A sequence number shifts under an
        // EXPUNGE from another client; a UID does not, which is why gtkmail
        // needs this one.
        const std::vector<jlib::net::imap::response> uids =
            imap.uid(*sock, "SEARCH", "ALL");

        unsigned long first_uid = 0;

        for(const jlib::net::imap::response& r : uids) {
            if(r.name() == "SEARCH" && !r.numbers().empty()) first_uid = r.numbers()[0];
        }

        ok("UID SEARCH answers with unique ids", first_uid != 0,
           std::to_string(first_uid));

        const std::vector<jlib::net::imap::response> byuid =
            imap.uid(*sock, "FETCH", std::to_string(first_uid) + " (RFC822.SIZE)");

        bool sized = false;

        for(const jlib::net::imap::response& r : byuid) {
            if(r.name() == "FETCH" && r.attributes().count("RFC822.SIZE")) sized = true;
        }

        ok("and UID FETCH takes one", sized);

        // A byte range.  PARTIAL was withdrawn in RFC 3501 in favour of this,
        // which is why the method that used to be called partial() is gone.
        // Eleven octets, which is "From: a@b.c" -- twelve would take the CR
        // as well, which is what the first draft of this line asked for and
        // then did not expect.
        const std::string head = imap.fetch_partial(*sock, 0, "", 0, 11);

        ok("a partial fetch returns only what was asked for",
           head == "From: a@b.c", "|" + show(head) + "|");

        const std::string middle = imap.fetch_partial(*sock, 0, "", 6, 5);

        ok("from the offset given", middle == "a@b.c", "|" + show(middle) + "|");

        // A server may return fewer octets than asked for; it may not return
        // more.
        ok("and no more than the message holds",
           imap.fetch_partial(*sock, 0, "", 0, 100000).size() == 36,
           std::to_string(imap.fetch_partial(*sock, 0, "", 0, 100000).size()));

        imap.logout(*sock);

        ok("LOGOUT", true);

        // The control.  Everything above says the new reader works; this says
        // the old one would not have, against this same server, rather than
        // asking anyone to take it on trust.
        //
        // A second connection, driven by hand, reading *lines* until one
        // begins with the tag -- which is exactly what Imap4::handshake did.
        {
            jlib::sys::socketstream raw("127.0.0.1", s.port);

            std::string line;

            jlib::sys::getline(raw, line);                       // greeting

            raw << "B1 LOGIN joe " << jlib::net::imap::quote(PASSWORD)
                << "\r\n" << std::flush;

            while(line.rfind("B1 ", 0) != 0) jlib::sys::getline(raw, line);

            raw << "B2 SELECT INBOX\r\n" << std::flush;

            while(line.rfind("B2 ", 0) != 0) jlib::sys::getline(raw, line);

            // Ask for the message whose body impersonates the tag, using that
            // very tag.
            raw << "A00001 FETCH 2 (RFC822)\r\n" << std::flush;

            std::string collected;
            int lines = 0;

            while(lines++ < 200) {
                jlib::sys::getline(raw, line);
                collected += line + "\r\n";

                if(line.rfind("A00001 ", 0) == 0) break;
            }

            ok("reading lines stops inside the message, as it always did",
               collected.find("A00020 OK FETCH completed") == std::string::npos,
               "stopped after " + std::to_string(lines) + " lines");

            // And the same fetch, read as responses, does not.
            jlib::sys::socketstream good("127.0.0.1", s.port);

            jlib::net::imap::read(good);

            good << "C1 LOGIN joe " << jlib::net::imap::quote(PASSWORD)
                 << "\r\n" << std::flush;

            while(jlib::net::imap::read(good).rfind("C1 ", 0) != 0) ;

            good << "C2 SELECT INBOX\r\n" << std::flush;

            while(jlib::net::imap::read(good).rfind("C2 ", 0) != 0) ;

            good << "A00001 FETCH 2 (RFC822)\r\n" << std::flush;

            const std::string whole = jlib::net::imap::read(good);

            ok("reading responses does not",
               whole.find("A00020 OK FETCH completed") != std::string::npos);
        }
    }
    catch(std::exception& e) {
        ok("the session ran without throwing", false, e.what());
        std::cerr << s.log();
    }

    // The same again over TLS, which is the difference between this library
    // being usable with a real account and not.
    //
    // "imaps" is the scheme IANA registered, and Imap4::is_secure() tested
    // find("simap") -- which "imaps" does not contain.  So an imaps:// URL was
    // not secure: the client opened a plain socket to port 143 and sent LOGIN
    // with the password on it.
    std::cout << "\nover TLS:\n";

    for(const char* scheme : { "imaps", "simap" }) {
        jlib::util::URL tls(std::string(scheme) + "://joe@localhost:"
                            + std::to_string(s.tls_port) + "/INBOX");

        tls.set_pass(PASSWORD);

        jlib::net::Imap4 secure(tls);

        ok(std::string(scheme) + ":// is a secure scheme", secure.is_secure());

        try {
            std::unique_ptr<jlib::sys::socketstream> sock(secure.connect());

            secure.login(*sock, "", "");
            secure.select(*sock, "INBOX");

            const std::string raw = secure.get(*sock, 1, false).raw();

            ok(std::string(scheme) + ":// handshakes, logs in and fetches",
               raw == impersonating_body(),
               raw == impersonating_body() ? std::string()
                                           : std::to_string(raw.size()) + " octets");

            secure.logout(*sock);
        }
        catch(std::exception& e) {
            ok(std::string(scheme) + ":// works", false, e.what());
            std::cerr << s.log();
        }
    }

    // STARTTLS, RFC 2595 3: the ordinary port, in the clear, upgraded in
    // place.  The same tlsstream primitive smtp::send_tls has used all along.
    std::cout << "\nSTARTTLS:\n";

    {
        jlib::util::URL up("imap://joe@localhost:" + std::to_string(s.port)
                           + "/INBOX?tls=starttls");

        up.set_pass(PASSWORD);

        jlib::net::Imap4 imap(up);

        ok("?tls=starttls asks for it", imap.use_starttls());
        ok("and it is not the same as a secure scheme", !imap.is_secure());

        try {
            std::unique_ptr<jlib::sys::socketstream> sock(imap.connect());

            // RFC 3501 6.2.1 requires the capability list be taken again after
            // the handshake, and a server must not still be offering STARTTLS
            // once it has been negotiated.
            ok("STARTTLS is gone from the list afterwards",
               !imap.has_capability("STARTTLS"));
            ok("and the list is not empty, so it was really re-issued",
               !imap.capabilities().empty(),
               std::to_string(imap.capabilities().size()) + " capabilities");

            imap.login(*sock, "", "");
            imap.select(*sock, "INBOX");

            const std::string raw = imap.get(*sock, 1, false).raw();

            ok("the session works over the upgraded connection",
               raw == impersonating_body(),
               raw == impersonating_body() ? std::string()
                                           : std::to_string(raw.size()) + " octets");

            imap.logout(*sock);
        }
        catch(std::exception& e) {
            ok("STARTTLS works", false, e.what());
            std::cerr << s.log();
        }
    }

    // Asked for and not offered is an error.  Carrying on in the clear is the
    // bug the top of this branch exists to fix, wearing a different hat -- so
    // the imaps port, which does not offer STARTTLS because it is already TLS,
    // has to be refused rather than used.
    {
        jlib::util::URL nope("imap://joe@localhost:" + std::to_string(s.tls_port)
                             + "/INBOX?tls=starttls");

        nope.set_pass(PASSWORD);

        bool threw = false;

        try {
            jlib::net::Imap4 imap(nope);
            std::unique_ptr<jlib::sys::socketstream> sock(imap.connect());
        }
        catch(std::exception&) { threw = true; }

        ok("asking for STARTTLS where it is not offered fails", threw);
    }

    // AUTHENTICATE against a server that decides for itself what it will
    // accept.  net_imap_sasl_test drives the same code against a scripted
    // server, which proves the framing; this proves the message.
    {
        jlib::util::URL u("imap://joe@127.0.0.1:" + std::to_string(s.port) + "/INBOX");

        u.set_pass(PASSWORD);

        jlib::net::Imap4 imap(u);

        try {
            std::unique_ptr<jlib::sys::socketstream> sock(imap.connect());

            imap.capability(*sock);

            ok("dovecot offers AUTH=PLAIN", imap.has_capability("AUTH=PLAIN"));

            // The same password as the LOGIN section: a space, a quote and a
            // backslash.  None of the three means anything inside a SASL
            // message -- the fields are counted, not delimited -- which is
            // half the reason to prefer this over LOGIN.
            imap.authenticate_plain(*sock);

            ok("AUTHENTICATE PLAIN succeeds against a real server",
               imap.state() == jlib::net::Imap4::Authenticated);

            imap.select(*sock, "INBOX");

            ok("and the session works afterwards", imap.exists() == 2,
               std::to_string(imap.exists()));

            imap.logout(*sock);
        }
        catch(std::exception& e) {
            ok("AUTHENTICATE PLAIN succeeds against a real server", false, e.what());
            std::cerr << s.log();
        }
    }

    // A mechanism the server has never heard of.  Dovecot answers a tagged NO
    // without ever sending a continuation, which is the one path the scripted
    // server cannot claim to have got right by construction.
    {
        jlib::util::URL u("imap://joe@127.0.0.1:" + std::to_string(s.port) + "/INBOX");

        u.set_pass(PASSWORD);

        jlib::net::Imap4 imap(u);

        try {
            std::unique_ptr<jlib::sys::socketstream> sock(imap.connect());

            bool threw = false;

            try {
                imap.authenticate(*sock, "NOSUCHMECHANISM",
                                  [](const std::string&) { return std::string(); });
            }
            catch(std::exception&) { threw = true; }

            ok("an unknown mechanism is refused", threw);

            // And the connection survives it, which is the assertion that
            // matters: a client that guesses at mechanisms has to be able to
            // fall back to the next one on the same socket.
            imap.authenticate_plain(*sock);

            ok("and the next mechanism works on the same connection",
               imap.state() == jlib::net::Imap4::Authenticated);

            imap.logout(*sock);
        }
        catch(std::exception& e) {
            ok("and the next mechanism works on the same connection", false, e.what());
            std::cerr << s.log();
        }
    }

    // OAuth2 end to end: jlib fetches an access token over HTTP, presents it
    // to dovecot over IMAP, and dovecot goes back over HTTP to check it.  Four
    // pieces of this branch's work in one line of a user's day.
    {
        jlib::net::oauth::client provider;

        provider.token_endpoint = "http://127.0.0.1:" +
                                  std::to_string(tokens.port()) + "/token";
        provider.id = "jlib-live-test";

        // Plaintext, which refresh() refuses by default and a real provider
        // would never offer.  It is loopback to a server in this process.
        provider.allow_http = true;

        std::string persisted;

        jlib::net::oauth::session account(provider, "good-refresh",
                                          [&persisted](const jlib::net::oauth::token& t) {
                                              persisted = t.refresh();
                                          });

        try {
            const std::string access = account.access();

            ok("an access token comes back from the token endpoint",
               access == "live-access-token", access);

            // Microsoft rotates on every refresh and kills the old one at the
            // same moment, so a client that cannot store the new one is logged
            // out after the first refresh.
            ok("and the rotated refresh token was handed over to be stored",
               persisted == "rotated-refresh", persisted);

            jlib::util::URL u("imap://joe@127.0.0.1:" + std::to_string(s.port) +
                              "/INBOX");

            jlib::net::Imap4 imap(u);

            std::unique_ptr<jlib::sys::socketstream> sock(imap.connect());

            imap.capability(*sock);

            ok("dovecot offers AUTH=XOAUTH2", imap.has_capability("AUTH=XOAUTH2"));

            imap.authenticate_xoauth2(*sock, "joe", access);

            ok("XOAUTH2 authenticates against a real server",
               imap.state() == jlib::net::Imap4::Authenticated);

            imap.select(*sock, "INBOX");

            ok("and the mailbox is there afterwards", imap.exists() == 2,
               std::to_string(imap.exists()));

            imap.logout(*sock);
        }
        catch(std::exception& e) {
            ok("XOAUTH2 authenticates against a real server", false, e.what());
            std::cerr << s.log();
        }
    }

    // A token the endpoint will not vouch for.  dovecot answers with a
    // continuation carrying base64 JSON and withholds the tagged NO until the
    // client replies -- which is the shape net_imap_sasl_test asserts against a
    // scripted server, here confirmed against a real one.
    {
        jlib::util::URL u("imap://joe@127.0.0.1:" + std::to_string(s.port) + "/INBOX");

        jlib::net::Imap4 imap(u);

        try {
            std::unique_ptr<jlib::sys::socketstream> sock(imap.connect());

            bool threw = false;

            try { imap.authenticate_xoauth2(*sock, "joe", "not-a-real-token"); }
            catch(std::exception&) { threw = true; }

            ok("a token the endpoint rejects is refused", threw);

            // The assertion that matters: the exchange finished, so the socket
            // is at a response boundary and can still be used.
            imap.authenticate_plain(*sock, "joe", mailserver::PASSWORD);

            ok("and the connection survives to try another mechanism",
               imap.state() == jlib::net::Imap4::Authenticated);

            imap.logout(*sock);
        }
        catch(std::exception& e) {
            ok("and the connection survives to try another mechanism", false,
               e.what());
            std::cerr << s.log();
        }
    }

    // The port it picks when it is not told one.
    ok("imaps:// with no port is still secure",
       jlib::net::Imap4(jlib::util::URL("imaps://joe@localhost/INBOX")).is_secure());

    ok("and imap:// is not",
       !jlib::net::Imap4(jlib::util::URL("imap://joe@localhost/INBOX")).is_secure());

    // What a green run does NOT establish.
    //
    // Not interoperability.  One server, one version, one configuration, with
    // TLS off and plaintext authentication on -- which is exactly what no real
    // account allows.  What it establishes is that jlib's reader survives a
    // response a real server chose to send as a literal.
    //
    // Not the commands #85 is about.  SEARCH, UID and a ranged FETCH are still
    // unimplemented; this exercises what was already there.
    //
    // Not a real provider.  The XOAUTH2 section above is end to end -- jlib
    // fetches a token over HTTP, dovecot validates it over HTTP, and the
    // mechanism in the middle is the one Gmail and Outlook.com require -- but
    // both endpoints are servers in this process, answering what this test told
    // them to.  Neither Google nor Microsoft can be reached from make check:
    // both need a registered application and a human at a browser.
    //
    // Nor TLS on the OAuth2 half.  The token endpoint here is plaintext
    // loopback with allow_http set, which is exactly what oauth::refresh
    // refuses by default.
    return failures ? 1 : 0;
}
