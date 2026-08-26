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

// RFC 3501 6.2.2's AUTHENTICATE exchange, against a server written here.
//
// A scripted server rather than dovecot, for the cases dovecot will not
// produce on demand: a challenge that arrives in two rounds, a server that
// challenges forever, a mechanism whose failure is reported as a base64 blob
// in a continuation rather than as a tagged NO.  Those are the shapes that
// break a client, and net_imap_live_test covers the ordinary path against a
// real one.
//
// The listener is sys::listener, which did not exist a branch ago; this test
// is the reason it was worth having.  The server runs on a thread rather than
// in a fork so it can record what the client actually sent and the assertions
// can look at it afterwards.
//
// What every section here is really testing is the same thing: that after the
// exchange, however it ended, the connection is still at a response boundary.
// A client that stops halfway through an AUTHENTICATE leaves the server
// waiting for a line, and the next command it sends is read as the answer to
// the challenge -- so every response after that belongs to the command before
// it.  That is the bug class the literal work fixed, arrived at from the other
// direction.

#include <jlib/net/Imap4.hh>

#include <jlib/sys/listener.hh>
#include <jlib/sys/socketstream.hh>

#include <jlib/util/URL.hh>
#include <jlib/util/util.hh>

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace util = jlib::util;

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
        else if(c == '\0') out += "\\0";
        else               out += c;
    }

    return out;
}

// A server that follows a script.
//
// The script is a list of lines to expect and lines to send: each entry sends
// its reply and then reads one line, which is recorded.  Everything the client
// sends ends up in sent(), including the "*" of a cancel, which is what most
// of the assertions here look at.
class scripted_server {
public:
    explicit scripted_server(std::vector<std::string> replies)
        : m_listener(),
          m_replies(std::move(replies))
    {
        m_thread = std::thread([this] { run(); });
    }

    ~scripted_server() {
        m_listener.close();

        if(m_thread.joinable()) m_thread.join();
    }

    unsigned short port() const { return m_listener.port(); }

    // Safe to call after join(); every section joins by letting the client's
    // socket close, which ends the server loop.
    const std::vector<std::string>& sent() {
        if(m_thread.joinable()) m_thread.join();

        return m_sent;
    }

private:
    void run() {
        try {
            std::unique_ptr<jlib::sys::socketstream> peer = m_listener.accept_stream(10);

            if(!peer) return;

            *peer << "* OK [CAPABILITY IMAP4rev1 AUTH=PLAIN] ready\r\n" << std::flush;

            for(const std::string& reply : m_replies) {
                std::string line;

                if(!std::getline(*peer, line)) return;

                while(!line.empty() && line.back() == '\r') line.pop_back();

                m_sent.push_back(line);

                *peer << reply << std::flush;
            }

            // Anything after the script is still recorded, so a section can
            // assert that the *next* command got its own response.
            for(;;) {
                std::string line;

                if(!std::getline(*peer, line)) return;

                while(!line.empty() && line.back() == '\r') line.pop_back();

                m_sent.push_back(line);

                const std::string tag = util::tokenize(line).empty()
                                        ? std::string("*") : util::tokenize(line)[0];

                *peer << tag << " OK done\r\n" << std::flush;
            }
        }
        catch(std::exception&) {
        }
    }

    jlib::sys::listener m_listener;
    std::vector<std::string> m_replies;
    std::vector<std::string> m_sent;
    std::thread m_thread;
};

// Every section drives the client through this.
//
// The read timeout is the point of it.  A driver that gets the exchange wrong
// does not fail here, it *deadlocks*: the server is waiting for the line the
// client owes it and the client is waiting for a response the server will not
// send until it arrives.  That was confirmed on this branch by taking the "*"
// out of cancel_authenticate() and running this test, which then hung until it
// was killed rather than reporting anything.  With the deadline in place the
// same mutation reports three failures in ten seconds, which is the difference
// between a test and a hang.
//
// A test that hangs on regression is a test that stops make check instead of
// failing it, so the socket gets a deadline -- sys::socketstream::set_timeout,
// which is a branch old.  Ten seconds is far longer than a loopback exchange
// needs and short enough that a broken driver reports rather than waits.
static std::unique_ptr<jlib::sys::socketstream> connect_to(jlib::net::Imap4& imap) {
    std::unique_ptr<jlib::sys::socketstream> sock(imap.connect());

    sock->set_timeout(10);

    return sock;
}

static jlib::util::URL url_for(unsigned short port) {
    jlib::util::URL u("imap://joe@127.0.0.1:" + std::to_string(port) + "/INBOX");

    u.set_pass("secret");

    return u;
}

static void plain_is_a_challenge_and_a_response() {
    std::cout << "\nAUTHENTICATE PLAIN is a challenge and a response:\n";

    // A CAPABILITY first, so the list this asserts about afterwards is one the
    // client really holds rather than one it never fetched.
    scripted_server server({ "* CAPABILITY IMAP4rev1 AUTH=PLAIN LOGINDISABLED\r\n"
                             "A00001 OK capability completed\r\n",
                             "+ \r\n",
                             "A00002 OK [CAPABILITY IMAP4rev1] logged in\r\n" });

    jlib::net::Imap4 imap(url_for(server.port()));

    try {
        std::unique_ptr<jlib::sys::socketstream> sock = connect_to(imap);

        imap.capability(*sock);

        ok("before: the server advertises AUTH=PLAIN and LOGINDISABLED",
           imap.has_capability("AUTH=PLAIN") && imap.has_capability("LOGINDISABLED"));

        imap.authenticate_plain(*sock, "joe", "secret");

        ok("it completes", true);
        ok("and leaves the client authenticated",
           imap.state() == jlib::net::Imap4::Authenticated);

        // 6.2.2: the capability list changes on authentication, which is why
        // the tagged OK is allowed to carry a new one.  Both of the two that
        // were there are gone -- and LOGINDISABLED being stale is not
        // cosmetic, it is login() refusing to send a password the server would
        // now accept.
        ok("after: the tagged OK's list replaces it",
           imap.has_capability("IMAP4rev1") &&
           !imap.has_capability("AUTH=PLAIN") &&
           !imap.has_capability("LOGINDISABLED"));

        sock->close();
    }
    catch(std::exception& e) {
        ok("it completes", false, e.what());
    }

    const std::vector<std::string>& sent = server.sent();

    ok("the command names the mechanism and nothing else",
       sent.size() >= 2 && sent[1] == "A00002 AUTHENTICATE PLAIN",
       sent.size() >= 2 ? show(sent[1]) : "");

    if(sent.size() >= 3) {
        const std::string message = util::base64::decode(sent[2]);

        // RFC 4616 2: authzid NUL authcid NUL passwd, with an empty authzid.
        ok("the response is the RFC 4616 message, base64",
           message == std::string("\0joe\0secret", 11), show(message));
    }
    else {
        ok("the response is the RFC 4616 message, base64", false, "nothing was sent");
    }
}

static void a_rejected_login_leaves_the_connection_usable() {
    std::cout << "\na rejected login leaves the connection usable:\n";

    scripted_server server({ "+ \r\n", "A00001 NO [AUTHENTICATIONFAILED] no\r\n" });

    jlib::net::Imap4 imap(url_for(server.port()));

    try {
        std::unique_ptr<jlib::sys::socketstream> sock = connect_to(imap);

        bool threw = false;

        try {
            imap.authenticate_plain(*sock, "joe", "wrong");
        }
        catch(std::exception&) {
            threw = true;
        }

        ok("a tagged NO throws", threw);
        ok("and the client is not authenticated",
           imap.state() != jlib::net::Imap4::Authenticated);

        // The point of the section.  If the driver had stopped without
        // finishing the exchange, this NOOP would be read as the answer to a
        // challenge and its response would never come.
        imap.noop(*sock);

        ok("a command afterwards gets its own response", true);

        sock->close();
    }
    catch(std::exception& e) {
        ok("a command afterwards gets its own response", false, e.what());
    }

    const std::vector<std::string>& sent = server.sent();

    ok("and that command went out under the next tag",
       sent.size() >= 3 && sent[2] == "A00002 NOOP",
       sent.size() >= 3 ? show(sent[2]) : "");
}

static void a_responder_that_gives_up_cancels() {
    std::cout << "\na responder that gives up cancels the exchange:\n";

    // The mechanism jlib cannot answer -- no credentials, an expired token,
    // an unparseable challenge.  The client has already sent AUTHENTICATE and
    // the server is waiting for a line; saying nothing is the one thing it
    // must not do.
    scripted_server server({ "+ Zm9v\r\n", "A00001 BAD authentication aborted\r\n" });

    jlib::net::Imap4 imap(url_for(server.port()));

    try {
        std::unique_ptr<jlib::sys::socketstream> sock = connect_to(imap);

        std::string seen;
        bool threw = false;

        try {
            imap.authenticate(*sock, "XOAUTH2",
                              [&seen](const std::string& challenge) -> std::string {
                                  seen = challenge;

                                  throw std::runtime_error("no token available");
                              });
        }
        catch(std::exception&) {
            threw = true;
        }

        ok("the responder's exception reaches the caller", threw);
        ok("and it saw the challenge decoded, not as base64", seen == "foo", show(seen));

        imap.noop(*sock);

        ok("a command afterwards gets its own response", true);

        sock->close();
    }
    catch(std::exception& e) {
        ok("a command afterwards gets its own response", false, e.what());
    }

    const std::vector<std::string>& sent = server.sent();

    ok("the client sent RFC 3501 6.2.2's \"*\"",
       sent.size() >= 2 && sent[1] == "*", sent.size() >= 2 ? show(sent[1]) : "");
    ok("and then the next command, under the next tag",
       sent.size() >= 3 && sent[2] == "A00002 NOOP",
       sent.size() >= 3 ? show(sent[2]) : "");
}

static void a_second_challenge_is_answered() {
    std::cout << "\na second challenge is answered:\n";

    // How XOAUTH2 reports a bad token: not as a tagged NO but as a
    // continuation carrying a JSON error, and the server will not send the
    // tagged NO until the client answers it -- with an empty line.  A driver
    // that handles exactly one round hangs here, holding a socket that looks
    // alive.
    const std::string error = "{\"status\":\"401\",\"schemes\":\"Bearer\"}";

    scripted_server server({ "+ \r\n",
                             "+ " + util::base64::encode(error) + "\r\n",
                             "A00001 NO invalid credentials\r\n" });

    jlib::net::Imap4 imap(url_for(server.port()));

    try {
        std::unique_ptr<jlib::sys::socketstream> sock = connect_to(imap);

        std::vector<std::string> challenges;
        bool threw = false;

        try {
            imap.authenticate(*sock, "XOAUTH2",
                              [&challenges](const std::string& challenge) -> std::string {
                                  challenges.push_back(challenge);

                                  // The first round sends the token; the
                                  // second acknowledges the error with an
                                  // empty line, which is what unblocks the
                                  // tagged NO.
                                  if(challenges.size() == 1) return "user=joe\001auth=Bearer t\001\001";

                                  return "";
                              });
        }
        catch(std::exception&) {
            threw = true;
        }

        ok("two rounds happen", challenges.size() == 2,
           std::to_string(challenges.size()));
        ok("the second carries the server's error, decoded",
           challenges.size() == 2 && challenges[1] == error,
           challenges.size() == 2 ? challenges[1] : "");
        ok("and the tagged NO is what throws", threw);

        imap.noop(*sock);

        ok("a command afterwards gets its own response", true);

        sock->close();
    }
    catch(std::exception& e) {
        ok("two rounds happen", false, e.what());
    }

    const std::vector<std::string>& sent = server.sent();

    ok("the empty response went out as an empty line",
       sent.size() >= 3 && sent[2].empty(),
       sent.size() >= 3 ? show(sent[2]) : "");
}

static void an_endless_challenge_is_not_endless() {
    std::cout << "\na server that challenges forever does not hang the client:\n";

    // Not a real server, but a client has no way to tell one from a broken one
    // and "loop until the peer stops" is not a termination argument.
    std::vector<std::string> replies;

    for(int i = 0; i < 40; i++) replies.push_back("+ \r\n");

    replies.push_back("A00001 BAD enough\r\n");

    scripted_server server(std::move(replies));

    jlib::net::Imap4 imap(url_for(server.port()));

    try {
        std::unique_ptr<jlib::sys::socketstream> sock = connect_to(imap);

        int rounds = 0;
        bool threw = false;

        try {
            imap.authenticate(*sock, "PLAIN", [&rounds](const std::string&) {
                rounds++;

                return "anything";
            });
        }
        catch(std::exception&) {
            threw = true;
        }

        ok("it gives up", threw);
        ok("after a bounded number of rounds", rounds > 0 && rounds <= 8,
           std::to_string(rounds));

        sock->close();
    }
    catch(std::exception& e) {
        ok("it gives up", false, e.what());
    }

    const std::vector<std::string>& sent = server.sent();

    bool cancelled = false;

    for(const std::string& line : sent) {
        if(line == "*") cancelled = true;
    }

    ok("and cancels rather than falling silent", cancelled);
}

static void a_nul_in_a_credential_is_refused() {
    std::cout << "\na NUL in a credential is refused before it is sent:\n";

    scripted_server server({ "+ \r\n", "A00001 OK in\r\n" });

    jlib::net::Imap4 imap(url_for(server.port()));

    try {
        std::unique_ptr<jlib::sys::socketstream> sock = connect_to(imap);

        bool threw = false;

        try {
            // RFC 4616's fields are NUL-separated, so a NUL inside one moves
            // where the next begins: "joe\0secret" as a username would
            // authenticate as joe with whatever followed.
            imap.authenticate_plain(*sock, std::string("jo\0e", 4), "secret");
        }
        catch(std::exception&) {
            threw = true;
        }

        ok("it throws", threw);

        sock->close();
    }
    catch(std::exception& e) {
        ok("it throws", false, e.what());
    }

    ok("and nothing at all was sent", server.sent().empty(),
       std::to_string(server.sent().size()) + " line(s)");
}

int main() {
    std::cout << std::unitbuf;

    plain_is_a_challenge_and_a_response();
    a_rejected_login_leaves_the_connection_usable();
    a_responder_that_gives_up_cancels();
    a_second_challenge_is_answered();
    an_endless_challenge_is_not_endless();
    a_nul_in_a_credential_is_refused();

    // What a green run does not establish: that a real server accepts what
    // this sends.  The server here answers what it was scripted to answer, so
    // it can prove the client's framing is right and cannot prove the
    // credential is well formed -- net_imap_live_test does that half against
    // dovecot.  Nor does it say anything about XOAUTH2 beyond the shape of the
    // exchange: the token in the second section is the literal string "t", and
    // no provider was asked whether the message around it is what they expect.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
