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

// The HTTP client, against a server in this process.
//
// Two kinds of assertion here.  What the client *reads* -- a chunked body, a
// 400 with a body worth having -- and what the client *sends*, which the
// server records and which is the half that cannot be checked any other way:
// that the query string reached the request-target, that Authorization came
// off across an origin change, that a POST body was not re-sent to a redirect.
//
// net_http_live_test is the other half, against a real nginx, including
// through a real proxy.

#include "httpserver.hh"

#include <jlib/net/http.hh>
#include <jlib/net/net.hh>

#include <jlib/util/URL.hh>

#include <iostream>
#include <map>
#include <string>

namespace http = jlib::net::http;

using jlib::util::URL;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static bool has_line(const std::string& head, const std::string& line) {
    return head.find(line + "\r\n") != std::string::npos;
}

static void a_request_and_a_response() {
    std::cout << "\na request and a response:\n";

    httpserver::server s([](const httpserver::request&) {
        return httpserver::reply(200, "OK", "hello",
                                 "Content-Type: text/plain\r\n");
    });

    const http::Response r = http::get(URL(s.url("/thing?a=1&b=two")));

    ok("the status is a number", r.status() == 200, std::to_string(r.status()));
    ok("the body arrives", r.body() == "hello", r.body());
    ok("and the fields", r.fields().get("Content-Type") == "text/plain");

    const std::vector<httpserver::request> sent = s.requests();

    ok("one request was made", sent.size() == 1, std::to_string(sent.size()));

    if(sent.empty()) return;

    // The request-target is path *and* query.  get_path() alone drops the
    // query string, which for half the requests this client makes is the whole
    // message -- and URL keeps the two apart, so it is easy to send only one.
    ok("the query string is in the request target",
       has_line(sent[0].head, "GET /thing?a=1&b=two HTTP/1.1"),
       sent[0].head.substr(0, sent[0].head.find("\r\n")));

    ok("Host names the authority",
       has_line(sent[0].head, "Host: 127.0.0.1:" + std::to_string(s.port())));

    // Not politeness: it ends the message at the end of the connection and
    // takes every keep-alive framing question off the table.
    ok("Connection: close is sent", has_line(sent[0].head, "Connection: close"));

    // RFC 9110 12.5.3 lets a server send any coding to a client that sends no
    // Accept-Encoding.  Nothing here can inflate a gzip body.
    ok("and Accept-Encoding: identity, so nothing arrives compressed",
       has_line(sent[0].head, "Accept-Encoding: identity"));

    // An empty path is "/" (RFC 9112 3.2.1).  "GET  HTTP/1.1" is not a
    // request line at all.
    httpserver::server bare([](const httpserver::request&) {
        return httpserver::reply(200, "OK");
    });

    http::get(URL("http://127.0.0.1:" + std::to_string(bare.port())));

    const std::vector<httpserver::request> b = bare.requests();

    ok("an empty path goes out as \"/\"",
       !b.empty() && has_line(b[0].head, "GET / HTTP/1.1"),
       b.empty() ? "" : b[0].head.substr(0, b[0].head.find("\r\n")));
}

static void a_non_2xx_is_not_an_exception() {
    std::cout << "\na non-2xx is not an exception:\n";

    // The whole reason this client exists.  A token endpoint answers 400 with
    // a JSON body saying whether the refresh token was revoked -- go and get
    // the user to authorize again -- or whether the server hiccuped and the
    // right move is to retry.  http::get() compared the status against the
    // string "200" and threw away everything else, so it could not tell those
    // apart and neither could its caller.
    const std::string error = "{\"error\":\"invalid_grant\"}";

    httpserver::server s([&error](const httpserver::request&) {
        return httpserver::reply(400, "Bad Request", error,
                                 "Content-Type: application/json\r\n");
    });

    bool threw = false;
    http::Response r;

    try {
        r = http::get(URL(s.url("/token")));
    }
    catch(std::exception&) {
        threw = true;
    }

    ok("it does not throw", !threw);
    ok("the status is there", r.status() == 400, std::to_string(r.status()));
    ok("ok() is false", !r.ok());
    ok("and the body is readable", r.body() == error, r.body());

    // 204 and 304 have no body whatever the fields say (RFC 9112 6.3).
    httpserver::server empty([](const httpserver::request&) {
        return std::string("HTTP/1.1 204 No Content\r\n\r\n");
    });

    const http::Response n = http::get(URL(empty.url()));

    ok("a 204 has no body and does not hang",
       n.status() == 204 && n.body().empty());
}

static void bodies_of_every_shape() {
    std::cout << "\nbodies of every shape:\n";

    {
        // The case that catches a chunked reader written with find(): the
        // chunk data contains a line that looks exactly like a chunk header.
        const std::string trap = "5\r\nnope\r\n";

        httpserver::server s([&trap](const httpserver::request&) {
            std::ostringstream o;

            o << "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
              << std::hex << trap.size() << "\r\n" << trap << "\r\n"
              << "3\r\nend\r\n"
              << "0\r\n\r\n";

            return o.str();
        });

        const http::Response r = http::get(URL(s.url()));

        ok("a chunked body reassembles, even one that looks like its own framing",
           r.body() == trap + "end");
    }

    {
        // No Content-Length and no Transfer-Encoding: the body is whatever
        // arrives before the connection closes.
        httpserver::server s([](const httpserver::request&) {
            return std::string("HTTP/1.1 200 OK\r\n\r\nto the very end");
        });

        const http::Response r = http::get(URL(s.url()));

        ok("a body with no framing runs to the close",
           r.body() == "to the very end", r.body());
    }

    {
        // RFC 9112 6.1, and this is the request-smuggling primitive: two
        // recipients disagreeing about where a message ends is how one request
        // becomes two.
        httpserver::server s([](const httpserver::request&) {
            return std::string("HTTP/1.1 200 OK\r\n"
                               "Content-Length: 5\r\n"
                               "Transfer-Encoding: chunked\r\n\r\n"
                               "0\r\n\r\n");
        });

        bool refused = false;

        try { http::get(URL(s.url())); }
        catch(http::error&) { refused = true; }

        ok("a response with both framing fields is refused", refused);
    }

    {
        // A Content-Length from a stranger decides an allocation.
        httpserver::server s([](const httpserver::request&) {
            return std::string("HTTP/1.1 200 OK\r\nContent-Length: 999999999\r\n\r\nx");
        });

        bool capped = false;
        http::options o;
        o.max_body = 1024;

        try { http::get(URL(s.url()), http::fields(), o); }
        catch(http::error&) { capped = true; }

        ok("and a body larger than the cap is refused rather than allocated", capped);
    }
}

static void posting_a_form() {
    std::cout << "\nposting a form:\n";

    // Every OAuth2 token request is one of these (RFC 6749 4.1.3).
    httpserver::server s([](const httpserver::request&) {
        return httpserver::reply(200, "OK", "{}");
    });

    std::map<std::string, std::string> form;

    form["grant_type"] = "refresh_token";
    form["refresh_token"] = "a+b/c=";
    form["scope"] = "mail read";

    http::post_form(URL(s.url("/token")), form);

    const std::vector<httpserver::request> sent = s.requests();

    ok("a POST was made", !sent.empty() && has_line(sent[0].head, "POST /token HTTP/1.1"));

    if(sent.empty()) return;

    ok("with a form content type",
       has_line(sent[0].head, "Content-Type: application/x-www-form-urlencoded"));

    ok("and a Content-Length",
       has_line(sent[0].head, "Content-Length: " + std::to_string(sent[0].body.size())));

    // The encoding is HTML's, not RFC 3986's: a space is "+", so a "+" in the
    // data has to be escaped.  That is the case that bites -- base64 produces
    // "+" constantly, and a refresh token sent through uri::encode arrives
    // with spaces in it and does not match.
    ok("a space is \"+\" and a \"+\" is escaped",
       sent[0].body.find("refresh_token=a%2Bb%2Fc%3D") != std::string::npos &&
       sent[0].body.find("scope=mail+read") != std::string::npos,
       sent[0].body);

    ok("form_encode agrees on its own",
       http::form_encode({ { "a", "x y" } }) == "a=x+y",
       http::form_encode({ { "a", "x y" } }));
}

static void what_it_refuses_to_send() {
    std::cout << "\nwhat it refuses to send:\n";

    httpserver::server s([](const httpserver::request&) {
        return httpserver::reply(200, "OK");
    });

    // A CR or LF in a field value is how one request becomes two.  Checked
    // against the grammar rather than against a list of bad characters:
    // field-name is a token and field-value is field-content, and those rules
    // are both stricter and easier to defend than a blocklist.
    http::fields bad;
    bad.add("X-Thing", "one\r\nX-Injected: two");

    bool refused = false;

    try { http::get(URL(s.url()), bad); }
    catch(http::error&) { refused = true; }

    ok("a header value containing CRLF", refused);

    http::fields worse;
    worse.add("X Thing", "one");

    refused = false;

    try { http::get(URL(s.url()), worse); }
    catch(http::error&) { refused = true; }

    ok("a header name that is not a token", refused);

    refused = false;

    try { http::request("GET IT", URL(s.url())); }
    catch(http::error&) { refused = true; }

    ok("a method that is not a token", refused);

    refused = false;

    try { http::get(URL("ftp://example.com/x")); }
    catch(http::error&) { refused = true; }

    ok("and a URL that is not http or https", refused);

    ok("nothing reached the server", s.requests().empty(),
       std::to_string(s.requests().size()) + " request(s)");
}

static void redirects_are_off_and_stay_off() {
    std::cout << "\nredirects are off, and what happens when they are on:\n";

    {
        // The one that matters.  Following a redirect on a POST means sending
        // the body -- a client_secret and a refresh_token -- to a host named
        // by the response.  A Location: is attacker-controlled the moment
        // anything upstream is.
        httpserver::server s([](const httpserver::request& r) {
            if(r.head.find("POST /token") == 0)
                return httpserver::reply(302, "Found", "",
                                         "Location: http://127.0.0.1:1/steal\r\n");

            return httpserver::reply(200, "OK", "should not get here");
        });

        const http::Response r = http::post_form(URL(s.url("/token")),
                                                 { { "secret", "hunter2" } });

        ok("a 302 on a POST is handed back, not followed", r.status() == 302);
        ok("and only the one request was made", s.requests().size() == 1,
           std::to_string(s.requests().size()));
    }

    {
        // With redirects on, a POST is still not followed, because re-sending
        // a body elsewhere is not a redirect.
        httpserver::server s([](const httpserver::request& r) {
            if(r.head.find("POST ") == 0)
                return httpserver::reply(307, "Temporary Redirect", "",
                                         "Location: /elsewhere\r\n");

            return httpserver::reply(200, "OK", "arrived");
        });

        http::options o;
        o.redirects = 3;

        const http::Response r = http::post_form(URL(s.url("/token")),
                                                 { { "secret", "hunter2" } }, http::fields(), o);

        ok("even with redirects on, a 307 on a POST is not followed",
           r.status() == 307 && s.requests().size() == 1);
    }

    {
        // A GET is, when asked, and an absolute path resolves against the same
        // origin without needing RFC 3986 5.3.
        httpserver::server s([](const httpserver::request& r) {
            if(r.head.find("GET /old") == 0)
                return httpserver::reply(302, "Found", "", "Location: /new?q=1\r\n");

            return httpserver::reply(200, "OK", "arrived");
        });

        http::options o;
        o.redirects = 3;

        const http::Response r = http::get(URL(s.url("/old")), http::fields(), o);

        ok("a GET follows a relative redirect when told to",
           r.status() == 200 && r.body() == "arrived", r.body());

        const std::vector<httpserver::request> sent = s.requests();

        ok("and the second request carries the new query string",
           sent.size() == 2 && has_line(sent[1].head, "GET /new?q=1 HTTP/1.1"),
           sent.size() >= 2 ? sent[1].head.substr(0, sent[1].head.find("\r\n")) : "");
    }

    {
        // 303 says "go and GET this instead" and drops the body.  RFC 9110
        // 15.4.4 -- this is the one redirect that legitimately changes a POST
        // into a GET.
        httpserver::server s([](const httpserver::request& r) {
            if(r.head.find("POST ") == 0)
                return httpserver::reply(303, "See Other", "", "Location: /result\r\n");

            return httpserver::reply(200, "OK", "result");
        });

        http::options o;
        o.redirects = 3;

        const http::Response r = http::post_form(URL(s.url("/submit")),
                                                 { { "a", "b" } }, http::fields(), o);

        const std::vector<httpserver::request> sent = s.requests();

        ok("a 303 turns a POST into a GET", r.status() == 200 && sent.size() == 2 &&
           has_line(sent[1].head, "GET /result HTTP/1.1"));

        ok("and does not carry the body", sent.size() == 2 && sent[1].body.empty());
    }
}

static void a_credential_does_not_cross_an_origin() {
    std::cout << "\na credential does not cross an origin:\n";

    // Two servers, so the redirect really does change origin.  Authorization
    // is scoped to one origin and to no other; a redirect that carried it
    // would be a way to read somebody's bearer token by sending them a 302.
    httpserver::server other([](const httpserver::request&) {
        return httpserver::reply(200, "OK", "arrived");
    });

    const std::string away = other.url("/elsewhere");

    httpserver::server first([&away](const httpserver::request&) {
        return httpserver::reply(302, "Found", "", "Location: " + away + "\r\n");
    });

    http::options o;
    o.redirects = 3;

    http::fields send;
    send.add("Authorization", "Bearer ya29.SECRET");
    send.add("X-Harmless", "kept");

    const http::Response r = http::get(URL(first.url("/start")), send, o);

    ok("the redirect was followed", r.status() == 200 && r.body() == "arrived");

    const std::vector<httpserver::request> at_first = first.requests();
    const std::vector<httpserver::request> at_other = other.requests();

    ok("the first origin got the Authorization",
       !at_first.empty() &&
       at_first[0].head.find("Authorization: Bearer ya29.SECRET") != std::string::npos);

    ok("and the second origin did not",
       !at_other.empty() &&
       at_other[0].head.find("Authorization") == std::string::npos);

    ok("but an ordinary field still travelled",
       !at_other.empty() && at_other[0].head.find("X-Harmless: kept") != std::string::npos);
}

static void the_proxy_parameter_is_read_one_way() {
    std::cout << "\nthe proxy parameter is read in one place:\n";

    std::string host;
    unsigned int port = 0;

    ok("no parameter is not an error",
       !jlib::net::proxy_of(URL("http://example.com/"), host, port));

    ok("host:port reads",
       jlib::net::proxy_of(URL("http://example.com/?proxy=p.example:3128"), host, port) &&
       host == "p.example" && port == 3128, host + ":" + std::to_string(port));

    // An IPv6 literal proxy has colons of its own, so the rightmost one is the
    // separator.  tokenize on ":" -- which is what Imap4 did -- would have
    // handed back "[" as the host.
    ok("an IPv6 literal reads",
       jlib::net::proxy_of(URL("http://example.com/?proxy=%5B::1%5D:3128"), host, port) &&
       host == "::1" && port == 3128, host + ":" + std::to_string(port));

    // Imap4 indexed pvec[1] with no size check, so this read off the end of a
    // vector.  Pop3 did not read the parameter at all and connected direct,
    // which is worse: a caller who asked for a proxy, perhaps because it is
    // the only route out, silently did not get one.
    for(const char* bad : { "host", "host:", ":3128", "host:nope", "host:0",
                            "host:99999" }) {
        bool threw = false;

        try {
            jlib::net::proxy_of(URL(std::string("http://example.com/?proxy=") + bad),
                                host, port);
        }
        catch(std::exception&) { threw = true; }

        ok(std::string("\"") + bad + "\" is refused rather than guessed at", threw);
    }
}

int main() {
    std::cout << std::unitbuf;

    a_request_and_a_response();
    a_non_2xx_is_not_an_exception();
    bodies_of_every_shape();
    posting_a_form();
    what_it_refuses_to_send();
    redirects_are_off_and_stay_off();
    a_credential_does_not_cross_an_origin();
    the_proxy_parameter_is_read_one_way();

    // What a green run does not establish.
    //
    // Not TLS.  Every server here is plaintext on loopback; https:// goes
    // through sys::tlsstream, which sys_tls_sigpipe_test and the mail live
    // tests exercise, but no test in this file makes an HTTPS request.
    //
    // Not a real server.  This one sends exactly what it was told to, which is
    // the point of it for the awkward cases and its whole limitation for the
    // ordinary ones -- net_http_live_test runs the same client against nginx,
    // and through tinyproxy, in the container.
    //
    // Not the redirect resolution jlib does not have.  RFC 3986 5.3 is not
    // implemented; a relative Location that is not an absolute path throws
    // rather than being guessed at, and that is asserted nowhere because
    // nothing has needed it.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
