/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * An http-to-https redirect, which is what a plaintext port is usually for.
 *
 * The section that matters most here is not the 301 -- it is that the redirect
 * is decided **before** the guards. `Basic` credentials are base64, so a 401
 * on a plaintext port asks for a password anyone on the path can read. A
 * server that redirected after authenticating would look identical in a
 * browser and would have handed the password over first.
 */

#include <jlib/net/http_server.hh>
#include <jlib/sys/sslstream.hh>
#include <jlib/sys/tls.hh>
#include <jlib/sys/listener.hh>
#include <jlib/sys/socketstream.hh>

#include "certificate.hh"

#include <cstdio>
#include <csignal>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace jlib;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Runs a server on its own thread and always joins it. */
struct turning {
    explicit turning(net::http::server& s) : m_s(s), m_t([&s] { s.run(); }) {}

    ~turning() { done(); }

    void done() {
        if(!m_t.joinable()) return;

        m_s.stop();
        m_t.join();
    }

    turning(const turning&) = delete;
    turning& operator=(const turning&) = delete;

private:
    net::http::server& m_s;
    std::thread        m_t;
};

/** Everything the server said to one raw request. */
static std::string raw(unsigned short port, const std::string& request) {
    try {
        sys::socketstream s("127.0.0.1", port, 5);

        s.set_timeout(5);
        s.write(request.data(), std::streamsize(request.size()));
        s.flush();

        std::ostringstream all;

        all << s.rdbuf();

        return all.str();
    }
    catch(std::exception&) {
        return std::string();
    }
}

static std::string get(unsigned short port, const std::string& target,
                       const std::string& host = "a.example",
                       const std::string& extra = "")
{
    return raw(port, "GET " + target + " HTTP/1.1\r\nHost: " + host + "\r\n" +
                     extra + "Connection: close\r\n\r\n");
}

static int status_of(const std::string& r) {
    if(r.size() < 12 || r.compare(0, 5, "HTTP/") != 0) return 0;

    return std::atoi(r.c_str() + 9);
}

/** The value of one field, or "". */
static std::string field_of(const std::string& r, const std::string& name) {
    const std::string want = "\r\n" + name + ":";
    const std::string::size_type at = r.find(want);

    if(at == std::string::npos) return std::string();

    const std::string::size_type from = at + want.size();
    const std::string::size_type end = r.find("\r\n", from);
    std::string value = r.substr(from, end - from);

    while(!value.empty() && value[0] == ' ') value.erase(0, 1);

    return value;
}

/** Two ports on one server: the first plaintext and redirecting, the second not. */
static void where_an_insecure_request_is_sent(bool async) {
    std::cout << (async ? "\nasync -- where it is sent:\n"
                        : "\nblocking -- where it is sent:\n");

    std::vector<sys::server::bound> ports;

    ports.push_back(sys::server::bound(sys::listener(0, "127.0.0.1")));
    ports.push_back(sys::server::bound(sys::listener(0, "127.0.0.1")));

    const unsigned short insecure = ports[0].l.port();
    const unsigned short other = ports[1].l.port();

    // Both listeners are plaintext here, deliberately: the thing under test is
    // that the *marker* decides, not the presence of a certificate.  A TLS
    // listener would mean a TLS client, which pins nothing extra and drags a
    // certificate into every assertion below.
    std::unique_ptr<net::http::server> s;

    if(async) {
        s.reset(new net::http::server(net::http::server::async_t(),
                                      std::move(ports)));
    }
    else {
        s.reset(new net::http::server(std::move(ports)));
    }

    s->route("GET", "/page", [](const util::http::Request&,
                                net::http::server::response& r) {
        r.status(200).type("text/plain").body("served\n");
    });

    s->redirect_insecure(insecure, 443);

    ok("  both ports are reported", s->ports().size() == 2,
       std::to_string(s->ports().size()));

    turning turn(*s);

    {
        const std::string r = get(insecure, "/page");

        ok("  an insecure request is moved permanently", status_of(r) == 301,
           std::to_string(status_of(r)));
        ok("  to the same name over https",
           field_of(r, "Location") == "https://a.example/page",
           field_of(r, "Location"));

        // 443 is the scheme's default, so naming it would be noise in every
        // address bar.
        ok("  with no port, since 443 is the default",
           field_of(r, "Location").find(":443") == std::string::npos,
           field_of(r, "Location"));
    }

    {
        // The other listener has no marker, so it serves.  Without this the
        // suite would pass with a redirect that fired on everything.
        const std::string r = get(other, "/page");

        ok("  a port with no marker still serves", status_of(r) == 200,
           std::to_string(status_of(r)));
        ok("  and says nothing about a location",
           field_of(r, "Location").empty(), field_of(r, "Location"));
    }

    {
        const std::string r = get(insecure, "/page?a=1&b=two");

        ok("  the query survives the move",
           field_of(r, "Location") == "https://a.example/page?a=1&b=two",
           field_of(r, "Location"));
    }

    {
        // A name nobody routed still redirects: where a request should have
        // gone does not depend on whether it would have been answered.
        const std::string r = get(insecure, "/nothing-here");

        ok("  a target that does not exist is moved, not 404ed",
           status_of(r) == 301, std::to_string(status_of(r)));
        ok("  and keeps the name it asked for",
           field_of(r, "Location") == "https://a.example/nothing-here",
           field_of(r, "Location"));
    }

    {
        const std::string r = get(insecure, "/page", "other.example");

        ok("  each name is sent to itself, without being configured",
           field_of(r, "Location") == "https://other.example/page",
           field_of(r, "Location"));
    }

    {
        // HTTP/1.0 with no Host has no authority to build a Location from.
        // Left alone rather than guessed at.
        const std::string r =
            raw(insecure, "GET /page HTTP/1.0\r\nConnection: close\r\n\r\n");

        ok("  a request with no name to send anywhere is served instead",
           status_of(r) == 200, std::to_string(status_of(r)));
    }

    turn.done();
}

/**
 * **The security property.**
 *
 * A guarded path on the insecure listener must redirect rather than
 * challenge. The failure this prevents is not an error page -- it is the
 * server asking for a password over cleartext, which a browser will happily
 * send.
 */
static void a_guard_is_not_offered_over_cleartext(bool async) {
    std::cout << (async ? "\nasync -- a guard on the insecure port:\n"
                        : "\nblocking -- a guard on the insecure port:\n");

    std::vector<sys::server::bound> ports;

    ports.push_back(sys::server::bound(sys::listener(0, "127.0.0.1")));
    ports.push_back(sys::server::bound(sys::listener(0, "127.0.0.1")));

    const unsigned short insecure = ports[0].l.port();
    const unsigned short plain = ports[1].l.port();

    std::unique_ptr<net::http::server> s;

    if(async) {
        s.reset(new net::http::server(net::http::server::async_t(),
                                      std::move(ports)));
    }
    else {
        s.reset(new net::http::server(std::move(ports)));
    }

    s->route("GET", "/private", [](const util::http::Request&,
                                   net::http::server::response& r) {
        r.status(200).type("text/plain").body("secret\n");
    });

    s->protect("/private", "Basic realm=\"test\"",
               [](const net::http::server::credentials& c) {
                   return c.user == "u" && c.password == "p";
               });

    s->redirect_insecure(insecure, 8443);

    turning turn(*s);

    {
        const std::string r = get(insecure, "/private");

        ok("  a guarded path is moved, not challenged", status_of(r) == 301,
           std::to_string(status_of(r)));

        // The assertion the whole file exists for.  A 401 here is the server
        // asking for a password in the clear.
        ok("  and no challenge is sent over cleartext",
           field_of(r, "WWW-Authenticate").empty(),
           field_of(r, "WWW-Authenticate"));

        ok("  the port is named when it is not 443",
           field_of(r, "Location") == "https://a.example:8443/private",
           field_of(r, "Location"));
    }

    {
        // Break-the-guard: the same path on a listener with no marker must
        // still challenge, or the section above would pass on a server whose
        // guards had simply stopped working.
        const std::string r = get(plain, "/private");

        ok("  while the unmarked port still challenges", status_of(r) == 401,
           std::to_string(status_of(r)));
        ok("  with a challenge to answer",
           !field_of(r, "WWW-Authenticate").empty(),
           field_of(r, "WWW-Authenticate"));
    }

    {
        // And credentials still work there, so the guard is whole rather than
        // merely refusing everything.
        const std::string r =
            get(plain, "/private", "a.example", "Authorization: Basic dTpw\r\n");

        ok("  and credentials still get through it", status_of(r) == 200,
           std::to_string(status_of(r)));
    }

    turn.done();
}


/**
 * A TLS listener does not redirect, even when it carries the marker's port.
 *
 * Without the `from.secure` test in redirected(), a request arriving over TLS
 * would be answered with a Location pointing at TLS -- which a browser follows
 * back to the same place, forever.  Every other section here uses plaintext
 * listeners, where `secure` is false throughout and that check cannot fail:
 * removing it left the whole file green, which is why this section exists.
 */
static void what_arrives_over_tls_stays(bool async) {
    std::cout << (async ? "\nasync -- arriving over TLS:\n"
                        : "\nblocking -- arriving over TLS:\n");

    const std::string cert = "http_redirect_cert.pem";
    const std::string key = "http_redirect_key.pem";

    if(!make_cert(cert, key)) {
        std::cout << "  skip  could not generate a test certificate\n";

        return;
    }

    const char* const had = std::getenv("SSL_CERT_FILE");
    const std::string keep = had ? had : "";

    ::setenv("SSL_CERT_FILE", cert.c_str(), 1);

    try {
        std::vector<sys::server::bound> ports;

        ports.push_back(sys::server::bound(sys::listener(0, "127.0.0.1")));
        ports.push_back(sys::server::bound(sys::listener(0, "127.0.0.1"),
                                           sys::tls_context::server(cert, key)));

        const unsigned short insecure = ports[0].l.port();
        const unsigned short secure = ports[1].l.port();

        std::unique_ptr<net::http::server> s;

        if(async) {
            s.reset(new net::http::server(net::http::server::async_t(),
                                          std::move(ports)));
        }
        else {
            s.reset(new net::http::server(std::move(ports)));
        }

        s->route("GET", "/page", [](const util::http::Request&,
                                    net::http::server::response& r) {
            r.status(200).type("text/plain").body("served\n");
        });

        // **Both ports marked**, which is the hostile arrangement: if the only
        // thing consulted were the port number, the TLS listener would send a
        // client back to itself.  What stops it is that the connection knows
        // it was already encrypted.
        s->redirect_insecure(insecure, secure);
        s->redirect_insecure(secure, secure);

        ok("  port() and tls() describe the first listener",
           s->port() == insecure && !s->tls());

        turning turn(*s);

        {
            const std::string r = get(insecure, "/page");

            ok("  the plaintext port is still moved", status_of(r) == 301,
               std::to_string(status_of(r)));
        }

        {
            sys::tlsstream c("localhost", secure);

            c.set_timeout(5);
            c << "GET /page HTTP/1.1\r\nHost: a.example\r\n"
              << "Connection: close\r\n\r\n" << std::flush;

            std::ostringstream all;

            all << c.rdbuf();

            const std::string r = all.str();

            ok("  but the TLS port serves rather than moving",
               status_of(r) == 200, std::to_string(status_of(r)));
            ok("  and sends nobody anywhere",
               field_of(r, "Location").empty(), field_of(r, "Location"));
        }

        turn.done();
    }
    catch(std::exception& e) {
        ok("  arriving over TLS", false, e.what());
    }

    if(had) ::setenv("SSL_CERT_FILE", keep.c_str(), 1);
    else    ::unsetenv("SSL_CERT_FILE");

    std::remove(cert.c_str());
    std::remove(key.c_str());
}

int main() {
    std::cout << std::unitbuf;

    // A client that hangs up on a server mid-write would otherwise take this
    // process down rather than the connection.
    std::signal(SIGPIPE, SIG_IGN);

    std::cout << "net_http_redirect_test\n";

    try {
        where_an_insecure_request_is_sent(false);
        where_an_insecure_request_is_sent(true);

        a_guard_is_not_offered_over_cleartext(false);
        a_guard_is_not_offered_over_cleartext(true);

        what_arrives_over_tls_stays(false);
        what_arrives_over_tls_stays(true);
    }
    catch(std::exception& e) {
        std::cerr << "net_http_redirect_test: " << e.what() << "\n";

        return 1;
    }

    if(failures) {
        std::cerr << "net_http_redirect_test: " << failures << " failed\n";

        return 1;
    }

    std::cout << "net_http_redirect_test: all good\n";

    // Not established here:
    //
    // Not 308, and not HSTS.  301 is what this sends and the only thing the
    // status line is checked against.
    return 0;
}
