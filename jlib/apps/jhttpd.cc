/* -*- mode: C++ c-basic-offset: 4  -*-
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
 *
 */

/**
 * jhttpd -- a web server you could point at a port and leave running.
 *
 * The first slice of #239, and deliberately a small one: it binds, serves a
 * directory, and **logs**. Logging is first because #239 puts it first, and
 * for the reason it gives -- a server you cannot see is a server you cannot
 * operate.
 *
 * ## What this is not, yet
 *
 * No config file, no virtual hosts, no SNI, no privilege dropping, no reload,
 * no directory index, nothing dynamic. Each of those is named in #239 and each
 * is its own piece of work. Running this on a public port is not advised and
 * the audit that would make it advisable is #270, which is deliberately not
 * started until there is more here to audit.
 *
 * ## Why the library does not do the logging
 *
 * `net::http::server` reports what happened through `on_request()` and formats
 * nothing. It cannot: it does not know whether there is a file, whether two
 * threads are writing to it, or what to do when the disk fills. This program
 * knows all three, and owns the escaping that makes a client-supplied field
 * safe to write down -- see jhttpd.hh, where that argument is made at length.
 */

#include <jlib/apps/jhttpd.hh>

#include <jlib/net/http_server.hh>
#include <jlib/sys/sys.hh>

#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

using namespace jlib;
using namespace jlib::net;

namespace {

struct options {
    unsigned short port = 8080;
    std::string    host = "127.0.0.1";
    std::string    root = ".";
    std::string    prefix = "/";
    std::string    access_log = "-";
    std::string    error_log = "-";
    std::string    cache_control;
    unsigned int   threads = 4;
    bool           async = false;
};

void usage(std::ostream& o, const char* argv0) {
    o << "usage: " << argv0 << " [options]\n"
      << "\n"
      << "  Serve a directory over HTTP, and write down what happened.\n"
      << "\n"
      << "  --root DIR        what to serve (default .)\n"
      << "  --prefix PATH     the URL prefix it is served under (default /)\n"
      << "  --port N          (default 8080)\n"
      << "  --host ADDR       (default 127.0.0.1; \"\" for every interface)\n"
      << "  --threads N       workers, 0 to serve inline (default 4)\n"
      << "  --async           one coroutine per connection instead of a pool\n"
      << "  --cache-control V sent with every file, e.g. \"max-age=3600\"\n"
      << "  --access-log F    Combined format; - is stdout, \"\" discards\n"
      << "  --error-log F     - is stderr, \"\" discards\n"
      << "  --help\n";
}

bool parse(int argc, char** argv, options& o) {
    for(int i = 1; i < argc; i++) {
        const std::string a = argv[i];

        if(a == "--help") { usage(std::cout, argv[0]); std::exit(0); }
        if(a == "--async") { o.async = true; continue; }

        if(a.size() > 2 && a.compare(0, 2, "--") == 0) {
            if(i + 1 >= argc) {
                std::cerr << "jhttpd: " << a << " wants a value\n";

                return false;
            }

            const std::string v = argv[++i];

            if(a == "--root") o.root = v;
            else if(a == "--prefix") o.prefix = v;
            else if(a == "--port") o.port = (unsigned short)std::atoi(v.c_str());
            else if(a == "--host") o.host = v;
            else if(a == "--threads") o.threads = unsigned(std::atoi(v.c_str()));
            else if(a == "--cache-control") o.cache_control = v;
            else if(a == "--access-log") o.access_log = v;
            else if(a == "--error-log") o.error_log = v;
            else {
                std::cerr << "jhttpd: unknown option " << a << "\n";

                return false;
            }

            continue;
        }

        std::cerr << "jhttpd: unexpected argument " << a << "\n";

        return false;
    }

    return true;
}

/** "/" becomes "/*"; "/x" becomes "/x/*"; a pattern given whole is kept. */
std::string pattern_for(const std::string& prefix) {
    if(!prefix.empty() && prefix[prefix.size() - 1] == '*') return prefix;

    std::string p = prefix;

    while(p.size() > 1 && p[p.size() - 1] == '/') p.erase(p.size() - 1);

    return (p == "/" ? std::string() : p) + "/*";
}

}

int main(int argc, char** argv) {
    options o;

    if(!parse(argc, argv, o)) return 2;

    // SIGPIPE from a client that goes away mid-response would kill the process
    // rather than failing the write.  Nothing here wants the default.
    std::signal(SIGPIPE, SIG_IGN);

    jhttpd::logfile access;
    jhttpd::logfile errors;

    if(o.access_log != "-" && !access.open(o.access_log)) {
        std::cerr << "jhttpd: cannot write the access log \"" << o.access_log
                  << "\"\n";

        return 1;
    }

    if(o.error_log != "-" && !errors.open(o.error_log)) {
        std::cerr << "jhttpd: cannot write the error log \"" << o.error_log
                  << "\"\n";

        return 1;
    }

    const bool access_to_stdout = o.access_log == "-";
    const bool errors_to_stderr = o.error_log == "-";

    try {
        sys::server::policy p;

        p.threads = o.threads;

        std::unique_ptr<http::server> s;

        if(o.async) {
            s.reset(new http::server(http::server::async_t(), o.port, o.host,
                                     sys::tls_context(), p));
        }
        else {
            s.reset(new http::server(o.port, o.host, sys::tls_context(), p));
        }

        s->files(pattern_for(o.prefix), o.root, o.cache_control);

        s->on_request([&access, access_to_stdout](
                          const http::server::access& a) {
            // std::time() here rather than in the library: when a request was
            // answered is a log's business, and a record that carried a
            // timestamp would be carrying one format's opinion.
            const std::string line = jhttpd::combined(a, std::time(0));

            if(access_to_stdout) std::cout << line << "\n" << std::flush;
            else                 access.write(line);
        });

        s->transport().on_error([&errors, errors_to_stderr](
                                    const std::exception& e,
                                    const sys::peer& from) {
            // Escaped for the same reason the access log is: this is where a
            // refused request's diagnosis arrives, and those messages quote
            // the offending value -- which the client chose.
            const std::string line =
                (from.address.empty() ? std::string("-") : from.address) +
                " " + jhttpd::escaped(e.what());

            if(errors_to_stderr) std::cerr << line << "\n";
            else                 errors.write(line);
        });

        std::cerr << "jhttpd: serving " << o.root << " on "
                  << (o.host.empty() ? "*" : o.host) << ":" << s->port()
                  << " under " << pattern_for(o.prefix)
                  << (o.async ? " (async)" : "") << "\n";

        s->run();
    }
    catch(std::exception& e) {
        std::cerr << "jhttpd: " << e.what() << "\n";

        return 1;
    }

    return 0;
}
