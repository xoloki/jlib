/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2002 Joey Yandle <xoloki@gmail.com>
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
 *
 */
/*
 * net_http_files_test -- serving a directory, and refusing to serve anything else.
 *
 * Most of this file is the second half.  Handing out a file is a dozen lines;
 * being sure that is the only thing handed out is the work, and it is the
 * first feature in this server with a way to be wrong that matters.
 *
 * Every refusal here is a 404 rather than a 403, deliberately, so the
 * assertions are about status codes that all look alike.  What distinguishes
 * them is what was asked for -- a traversal, a symlink out, a directory -- and
 * each is arranged on disk rather than described.
 */

#include <jlib/net/http.hh>
#include <jlib/net/http_server.hh>
#include <jlib/util/URL.hh>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

using namespace jlib;
using namespace jlib::net;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static void write_file(const std::string& path, const std::string& text) {
    std::ofstream o(path.c_str(), std::ios::binary);

    o << text;
}

/** A root to serve, and a sibling nobody should ever reach. */
struct tree {
    std::string base;
    std::string root;
    std::string secret;

    tree() {
        char pattern[] = "/tmp/jlib_files_XXXXXX";

        base = ::mkdtemp(pattern);
        root = base + "/www";
        secret = base + "/secret.txt";

        ::mkdir(root.c_str(), 0755);
        ::mkdir((root + "/sub").c_str(), 0755);

        write_file(root + "/index.html", "<h1>hello</h1>\n");
        write_file(root + "/app.js", "console.log(1)\n");
        write_file(root + "/sub/deep.txt", "deep\n");
        write_file(root + "/nosuffix", "no extension here\n");
        write_file(secret, "you should never see this\n");

        // Two symlinks: one that stays inside, one that leaves.
        ::symlink((root + "/index.html").c_str(), (root + "/inside.html").c_str());
        ::symlink(secret.c_str(), (root + "/escape.txt").c_str());
    }

    ~tree() {
        // Best effort; these are under /tmp and small.
        const std::string cmd = "rm -rf '" + base + "'";

        if(::system(cmd.c_str()) != 0) { /* nothing useful to do */ }
    }
};

static util::http::Response ask(http::server& s, const std::string& method,
                                const std::string& path)
{
    return http::request(method, util::URL(s.url(path)));
}

static util::http::Response ask_with(http::server& s, const std::string& method,
                                     const std::string& path,
                                     const std::string& name,
                                     const std::string& value)
{
    util::http::fields f;

    f.add(name, value);

    return http::request(method, util::URL(s.url(path)), f);
}

static void what_it_serves(http::server& s) {
    std::cout << "\nwhat it serves:\n";

    {
        const util::http::Response r = ask(s, "GET", "/static/index.html");

        ok("  a file under the root", r.status() == 200 &&
           r.body() == "<h1>hello</h1>\n", r.body());

        ok("  with a type from its extension",
           util::http::fold(r.fields().get("Content-Type"))
               .find("text/html") == 0, r.fields().get("Content-Type"));

        ok("  and validators the server made itself",
           r.fields().has("ETag") && r.fields().has("Last-Modified"),
           r.fields().get("ETag") + " / " + r.fields().get("Last-Modified"));

        // **Weak, and it says so.**  mtime and size cannot promise the bytes
        // are identical, only that they are probably the same file.
        ok("  the ETag is weak, because mtime and size cannot promise more",
           r.fields().get("ETag").compare(0, 2, "W/") == 0,
           r.fields().get("ETag"));
    }

    {
        const util::http::Response r = ask(s, "GET", "/static/sub/deep.txt");

        ok("  a file further down", r.status() == 200 && r.body() == "deep\n",
           r.body());
    }

    {
        const util::http::Response r = ask(s, "GET", "/static/app.js");

        ok("  javascript by extension, not by sniffing its contents",
           util::http::fold(r.fields().get("Content-Type"))
               .find("application/javascript") == 0,
           r.fields().get("Content-Type"));
    }

    {
        const util::http::Response r = ask(s, "GET", "/static/nosuffix");

        // The one answer that cannot be wrong dangerously: a browser will not
        // execute an octet-stream.
        ok("  and no extension means octet-stream",
           r.fields().get("Content-Type") == "application/octet-stream",
           r.fields().get("Content-Type"));
    }

    {
        const util::http::Response r = ask(s, "GET", "/static/inside.html");

        ok("  a symlink that stays inside the root is fine",
           r.status() == 200 && r.body() == "<h1>hello</h1>\n",
           std::to_string(r.status()));
    }
}

static void what_it_refuses(http::server& s, const tree& t) {
    std::cout << "\nwhat it refuses, all as 404:\n";

    struct { const char* path; const char* why; } cases[] = {
        { "/static/../secret.txt",          "a traversal" },
        { "/static/sub/../../secret.txt",   "a traversal from further down" },
        { "/static/escape.txt",             "a symlink pointing out of the root" },
        { "/static/sub",                    "a directory" },
        { "/static/",                       "the root itself" },
        { "/static/nope.txt",               "something that is not there" }
    };

    for(std::size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const util::http::Response r = ask(s, "GET", cases[i].path);

        ok(std::string("  ") + cases[i].why, r.status() == 404,
           std::string(cases[i].path) + " -> " + std::to_string(r.status()));

        // The stronger half: whatever the status, the secret is not in it.
        ok(std::string("  ") + cases[i].why + ", and says nothing about it",
           r.body().find("never see this") == std::string::npos,
           r.body().substr(0, 40));
    }

    (void)t;
}

static void the_conditional_and_head_paths(http::server& s) {
    std::cout << "\nwhat the rest of the server already does for it:\n";

    const util::http::Response first = ask(s, "GET", "/static/index.html");

    {
        // Nothing in files() implements this: the ETag it produced is enough.
        const util::http::Response r =
            ask_with(s, "GET", "/static/index.html", "If-None-Match",
                     first.fields().get("ETag"));

        ok("  the ETag it handed out comes back as a 304", r.status() == 304,
           std::to_string(r.status()));
    }

    {
        const util::http::Response r =
            ask_with(s, "GET", "/static/index.html", "If-Modified-Since",
                     first.fields().get("Last-Modified"));

        ok("  and so does the Last-Modified", r.status() == 304,
           std::to_string(r.status()));
    }

    {
        const util::http::Response r = ask(s, "HEAD", "/static/index.html");

        ok("  HEAD works without files() knowing about it",
           r.status() == 200 && r.body().empty(),
           std::to_string(r.status()));

        ok("  with the length the body would have had",
           r.fields().get("Content-Length") ==
           std::to_string(first.body().size()),
           r.fields().get("Content-Length"));
    }
}

static void a_root_that_is_not_there() {
    std::cout << "\na root that cannot be resolved:\n";

    http::server s(0, "127.0.0.1");

    bool threw = false;
    std::string why;

    try { s.files("/static/*", "/no/such/directory/anywhere"); }
    catch(std::exception& e) { threw = true; why = e.what(); }

    // At registration, where the caller is -- not as a 404 per request
    // forever, which is what not checking would look like.
    ok("  is refused when the route is registered", threw, why);
}

int main() {
    std::cout << "net_http_files_test\n";

    try {
        tree t;

        {
            http::server s(0, "127.0.0.1");

            s.files("/static/*", t.root);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            std::thread th([&s]{ s.run(); });

            std::cout << "\n-- the blocking server --\n";
            what_it_serves(s);
            what_it_refuses(s, t);
            the_conditional_and_head_paths(s);

            s.stop();
            th.join();
        }

        {
            http::server s(http::server::async_t(), 0, "127.0.0.1");

            s.files("/static/*", t.root);
            s.transport().on_error([](const std::exception&, const sys::peer&) {});

            std::thread th([&s]{ s.run(); });

            std::cout << "\n-- the async server --\n";
            what_it_serves(s);
            what_it_refuses(s, t);
            the_conditional_and_head_paths(s);

            s.stop();
            th.join();
        }

        a_root_that_is_not_there();
    }
    catch(std::exception& e) {
        std::cerr << "net_http_files_test: " << e.what() << "\n";
        return 1;
    }

    if(failures) {
        std::cerr << "net_http_files_test: " << failures << " failed\n";
        return 1;
    }

    std::cout << "net_http_files_test: all good\n";

    return 0;
}
