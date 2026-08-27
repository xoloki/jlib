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

// The HTTP client against a server nobody here wrote.
//
// net_http_test drives it against a server written in the test, which is the
// only way to get the awkward shapes -- a chunked body whose data looks like
// its own framing, both framing fields at once -- and which for that same
// reason can only ever send what somebody thought of.  nginx sends a Date and
// a Server and an ETag and a 404 body that nobody here chose, in an order
// nobody here chose, and that is the whole point of it.
//
// And through tinyproxy, which is already in the image for the mail tests: the
// CONNECT path and the plaintext proxy path are different code, and neither
// had ever carried an HTTP request.
//
// SKIP (77) where there is no nginx, which is every machine that is not the
// build container.

#include <jlib/net/http.hh>
#include <jlib/net/net.hh>

#include <jlib/sys/socketstream.hh>
#include <jlib/sys/sys.hh>

#include <jlib/util/URL.hh>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
namespace http = jlib::net::http;

using jlib::util::URL;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** nginx and tinyproxy, started for this test and stopped after it. */
struct site {
    fs::path dir;
    unsigned int port = 0;
    unsigned int proxy_port = 0;
    bool running = false;
    bool proxied = false;

    ~site() { stop(); }

    bool start() {
        std::string out, err;

        try {
            if(jlib::sys::run({ "nginx", "-v" }, out, err) != 0) return false;
        }
        catch(std::exception&) {
            return false;
        }

        // Derived from the pid, so two builds on one machine do not collide.
        port = 16000 + static_cast<unsigned int>(::getpid() % 900);
        proxy_port = port + 1000;

        dir = fs::temp_directory_path() / ("jlib-http-" + std::to_string(::getpid()));

        fs::remove_all(dir);
        fs::create_directories(dir / "www");
        fs::create_directories(dir / "logs");
        fs::create_directories(dir / "tmp");

        {
            std::ofstream f(dir / "www" / "hello.txt");
            f << "hello from nginx\n";
        }

        {
            std::ofstream f(dir / "nginx.conf");

            f << "daemon on;\n"
              << "worker_processes 1;\n"
              << "pid " << (dir / "nginx.pid").string() << ";\n"
              << "error_log " << (dir / "logs" / "error.log").string() << ";\n"
              << "events { worker_connections 64; }\n"
              << "http {\n"
              << "  access_log " << (dir / "logs" / "access.log").string() << ";\n"
              << "  client_body_temp_path " << (dir / "tmp").string() << ";\n"
              << "  proxy_temp_path " << (dir / "tmp").string() << ";\n"
              << "  fastcgi_temp_path " << (dir / "tmp").string() << ";\n"
              << "  uwsgi_temp_path " << (dir / "tmp").string() << ";\n"
              << "  scgi_temp_path " << (dir / "tmp").string() << ";\n"
              << "  default_type application/octet-stream;\n"
              << "  server {\n"
              << "    listen 127.0.0.1:" << port << ";\n"
              << "    root " << (dir / "www").string() << ";\n"
              << "    location = /echo {\n"
              << "      return 200 \"$request_method $args\";\n"
              << "    }\n"
              << "    location = /moved {\n"
              << "      return 302 \"/hello.txt\";\n"
              << "    }\n"
              << "    location = /teapot {\n"
              << "      return 418 \"short and stout\";\n"
              << "    }\n"
              << "  }\n"
              << "}\n";
        }

        if(jlib::sys::run({ "nginx", "-c", (dir / "nginx.conf").string(),
                            "-p", dir.string() }, out, err) != 0) {
            std::cerr << "nginx would not start: " << err << out << "\n";

            return false;
        }

        running = true;

        for(int i = 0; i < 100; i++) {
            try {
                jlib::sys::socketstream probe("127.0.0.1", port);

                return true;
            }
            catch(std::exception&) {
                ::usleep(50000);
            }
        }

        std::cerr << "nginx did not come up on " << port << "\n";

        return false;
    }

    bool start_proxy() {
        std::string out, err;

        try {
            if(jlib::sys::run({ "tinyproxy", "-v" }, out, err) != 0) return false;
        }
        catch(std::exception&) {
            return false;
        }

        {
            std::ofstream f(dir / "proxy.conf");

            f << "Port " << proxy_port << "\n"
              << "Listen 127.0.0.1\n"
              << "Timeout 60\n"
              << "LogFile \"" << (dir / "logs" / "proxy.log").string() << "\"\n"
              << "LogLevel Info\n"
              << "MaxClients 20\n"
              << "Allow 127.0.0.1\n"
              << "ConnectPort " << port << "\n";
        }

        if(jlib::sys::run({ "tinyproxy", "-c", (dir / "proxy.conf").string() },
                          out, err) != 0) {
            std::cerr << "tinyproxy would not start: " << err << out << "\n";

            return false;
        }

        proxied = true;

        for(int i = 0; i < 100; i++) {
            try {
                jlib::sys::socketstream probe("127.0.0.1", proxy_port);

                return true;
            }
            catch(std::exception&) {
                ::usleep(50000);
            }
        }

        return false;
    }

    void stop() {
        std::string out, err;

        if(running) {
            try {
                jlib::sys::run({ "nginx", "-c", (dir / "nginx.conf").string(),
                                 "-p", dir.string(), "-s", "quit" }, out, err);
            }
            catch(std::exception&) {}

            running = false;
        }

        if(proxied) {
            try { jlib::sys::run({ "pkill", "-f", (dir / "proxy.conf").string() }, out, err); }
            catch(std::exception&) {}

            proxied = false;
        }

        if(!dir.empty()) {
            std::error_code e;
            fs::remove_all(dir, e);
        }
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }

    std::string through_proxy(const std::string& path) const {
        return url(path) + "?proxy=127.0.0.1:" + std::to_string(proxy_port);
    }
};

int main() {
    std::cout << std::unitbuf;

    site s;

    if(!s.start()) {
        std::cerr << "no nginx here; skipping\n";

        return 77;
    }

    std::cout << "nginx on 127.0.0.1:" << s.port << "\n";

    std::cout << "\nagainst a real server:\n";

    try {
        const http::Response r = http::get(URL(s.url("/hello.txt")));

        ok("a GET of a static file", r.status() == 200 &&
           r.body() == "hello from nginx\n", r.body());

        // Fields nobody here chose, in an order nobody here chose.  An
        // in-process server sends the two or three a test thought of.
        ok("the fields a real server sends are all read",
           r.fields().has("Server") && r.fields().has("Date") &&
           r.fields().has("Content-Type") && r.fields().has("Last-Modified") &&
           r.fields().has("ETag"),
           std::to_string(r.fields().size()) + " fields");

        ok("and Content-Length framed the body",
           r.body_framing() == http::framing::length &&
           r.content_length() == r.body().size());
    }
    catch(std::exception& e) {
        ok("a GET of a static file", false, e.what());
    }

    try {
        const http::Response r = http::get(URL(s.url("/nothing-here")));

        // A 404 is a response with a body, not an error.  http::get() compared
        // the status against the string "200" and threw everything else away.
        ok("a 404 is a response with a body", r.status() == 404 &&
           !r.ok() && !r.body().empty(),
           std::to_string(r.body().size()) + " octets");
    }
    catch(std::exception& e) {
        ok("a 404 is a response with a body", false, e.what());
    }

    try {
        const http::Response r = http::get(URL(s.url("/teapot")));

        ok("and so is a status nobody has a branch for",
           r.status() == 418 && r.body() == "short and stout", r.body());
    }
    catch(std::exception& e) {
        ok("and so is a status nobody has a branch for", false, e.what());
    }

    try {
        // The query string has to reach the request-target: nginx echoes $args
        // back, which is what it parsed out of the request line this client
        // built.
        const http::Response r = http::get(URL(s.url("/echo?a=1&b=two")));

        ok("the query string arrives at the server",
           r.body() == "GET a=1&b=two", r.body());
    }
    catch(std::exception& e) {
        ok("the query string arrives at the server", false, e.what());
    }

    try {
        const http::Response off = http::get(URL(s.url("/moved")));

        ok("a redirect is handed back by default", off.status() == 302 &&
           off.fields().has("Location"), off.fields().get("Location"));

        http::options o;
        o.redirects = 3;

        const http::Response on = http::get(URL(s.url("/moved")), http::fields(), o);

        ok("and followed when asked", on.status() == 200 &&
           on.body() == "hello from nginx\n", on.body());
    }
    catch(std::exception& e) {
        ok("a redirect is handed back by default", false, e.what());
    }

    try {
        const http::Response r = http::post_form(URL(s.url("/echo")),
                                                 { { "grant_type", "refresh_token" } });

        ok("a POST with a form body reaches it", r.status() == 200 &&
           r.body().substr(0, 4) == "POST", r.body());
    }
    catch(std::exception& e) {
        ok("a POST with a form body reaches it", false, e.what());
    }

    if(s.start_proxy()) {
        std::cout << "\nthrough a real proxy on " << s.proxy_port << ":\n";

        try {
            const http::Response r = http::get(URL(s.through_proxy("/hello.txt")));

            ok("a request through a CONNECT proxy", r.status() == 200 &&
               r.body() == "hello from nginx\n", r.body());
        }
        catch(std::exception& e) {
            ok("a request through a CONNECT proxy", false, e.what());
        }

        try {
            // tinyproxy is configured to allow CONNECT only to nginx's port,
            // so this is refused -- and the refusal has to reach the caller
            // rather than becoming a mysterious connection failure.
            bool threw = false;
            std::string why;

            try {
                http::get(URL("http://127.0.0.1:9/x?proxy=127.0.0.1:" +
                              std::to_string(s.proxy_port)));
            }
            catch(std::exception& e) {
                threw = true;
                why = e.what();
            }

            ok("and a proxy that refuses the tunnel says so", threw, why);
        }
        catch(std::exception& e) {
            ok("and a proxy that refuses the tunnel says so", false, e.what());
        }
    }
    else {
        std::cout << "\n  skip  no tinyproxy; the proxy path is untested here\n";
    }

    // What a green run does not establish.
    //
    // Not TLS.  nginx here is plaintext; https:// goes through sys::tlsstream,
    // which the mail live tests exercise against dovecot, but no request in
    // this file is an HTTPS one.
    //
    // Not interoperability.  One server, one version, one configuration.  What
    // it establishes is that the client's request line, its Host, and its
    // reading of a response head are acceptable to something that was not
    // written to accept them.
    //
    // Not the awkward shapes.  A chunked body whose data looks like its own
    // framing, both framing fields at once, a header section with no end --
    // nginx will not send any of those, which is why net_http_test exists.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
