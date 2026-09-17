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
#include <list>
#include <termios.h>
#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

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

    std::string    cert;
    std::string    key;

    // Zero is off for both, which is what the library defaults to.  jhttpd
    // does not pick a limit on the operator's behalf: a rate that is wrong is
    // worse than none, because it is wrong in a way that looks deliberate.
    double         rate = 0;
    double         burst = 0;
    std::size_t    max_per_address = 0;

    // These do have defaults, because they are the library's and a server
    // that quietly widened them would be removing a bound somebody is
    // relying on.  Mirrored here so --help can print them.
    std::size_t    max_connections = 256;
    double         io_timeout = 30;
    double         initial_idle_timeout = 5;
    double         idle_timeout = 60;
    std::size_t    max_requests = 100;

    // PREFIX:REALM:FILE, in the order they were given.  A list rather than one,
    // because a server with a private area usually has more than one of them.
    std::vector<std::string> protect;

    bool           hash_password = false;
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
      << "\n"
      << "  TLS:\n"
      << "  --cert FILE       certificate chain, PEM\n"
      << "  --key FILE        private key, PEM\n"
      << "\n"
      << "  What one client may have:\n"
      << "  --rate N          requests per second per address (0 is off)\n"
      << "  --burst N         how many may arrive at once (default: the rate)\n"
      << "  --max-per-address N   connections one address may hold (0 is off)\n"
      << "\n"
      << "  What every client together may have:\n"
      << "  --max-connections N   (default 256, async only)\n"
      << "  --max-requests N      per connection (default 100)\n"
      << "  --io-timeout F        seconds for one request (default 30)\n"
      << "  --initial-idle F      a new connection that says nothing (default 5)\n"
      << "  --idle-timeout F      between requests on a kept connection (default 60)\n"
      << "\n"
      << "  Authentication:\n"
      << "  --protect PREFIX:REALM:FILE\n"
      << "                    require a credential under PREFIX.  FILE holds\n"
      << "                    one user:hash line each, Argon2id, mode 600.\n"
      << "  --hash-password   read a password and print a line for that file\n"
      << "\n"
      << "  SIGHUP reopens the log files, for rotation.\n"
      << "\n"
      << "  --help\n";
}

bool parse(int argc, char** argv, options& o) {
    for(int i = 1; i < argc; i++) {
        const std::string a = argv[i];

        if(a == "--help") { usage(std::cout, argv[0]); std::exit(0); }
        if(a == "--async") { o.async = true; continue; }
        if(a == "--hash-password") { o.hash_password = true; continue; }

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
            else if(a == "--cert") o.cert = v;
            else if(a == "--key") o.key = v;
            else if(a == "--rate") o.rate = std::atof(v.c_str());
            else if(a == "--burst") o.burst = std::atof(v.c_str());
            else if(a == "--max-per-address")
                o.max_per_address = std::size_t(std::atol(v.c_str()));
            else if(a == "--max-connections")
                o.max_connections = std::size_t(std::atol(v.c_str()));
            else if(a == "--max-requests")
                o.max_requests = std::size_t(std::atol(v.c_str()));
            else if(a == "--io-timeout") o.io_timeout = std::atof(v.c_str());
            else if(a == "--initial-idle")
                o.initial_idle_timeout = std::atof(v.c_str());
            else if(a == "--idle-timeout") o.idle_timeout = std::atof(v.c_str());
            else if(a == "--protect") o.protect.push_back(v);
            else {
                std::cerr << "jhttpd: unknown option " << a << "\n";

                return false;
            }

            continue;
        }

        std::cerr << "jhttpd: unexpected argument " << a << "\n";

        return false;
    }

    // A certificate without a key is a typo, not a configuration.  Refused
    // here rather than at the first connection, which is the argument
    // files() and protect() both make about registration.
    if(o.cert.empty() != o.key.empty()) {
        std::cerr << "jhttpd: --cert and --key go together\n";

        return false;
    }

    // A burst that is not said defaults to the rate, which is the shape that
    // surprises nobody: "ten a second" allows ten at once.  A burst below one
    // token would refuse everything, and the library clamps it, but saying so
    // here is better than being clamped silently.
    if(o.rate > 0 && o.burst <= 0) o.burst = o.rate;

    return true;
}

/**
 * Split `PREFIX:REALM:FILE` into three.
 *
 * From the right, because a **path may contain a colon** and so may a realm --
 * `/a:b/*:My Realm:/etc/creds` is a legal thing to want. The file is after the
 * last colon and the realm after the one before it, which is the only reading
 * that does not refuse a legal prefix.
 */
bool split_protect(const std::string& spec, std::string& prefix,
                   std::string& realm, std::string& file)
{
    const std::string::size_type last = spec.rfind(':');

    if(last == std::string::npos || last == 0) return false;

    const std::string::size_type mid = spec.rfind(':', last - 1);

    if(mid == std::string::npos || mid == 0) return false;

    prefix = spec.substr(0, mid);
    realm = spec.substr(mid + 1, last - mid - 1);
    file = spec.substr(last + 1);

    return !prefix.empty() && !realm.empty() && !file.empty();
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

    // Before anything is bound or opened: this reads a password and prints a
    // line, and is not a server at all.
    if(o.hash_password) {
        std::string pw;

        // No echo if this is a terminal, and no prompt if it is not -- so it
        // composes with a pipe without printing a prompt into the output.
        const bool tty = ::isatty(STDIN_FILENO) != 0;

        if(tty) {
            std::cerr << "password: ";

            struct termios was;

            if(::tcgetattr(STDIN_FILENO, &was) == 0) {
                struct termios quiet = was;

                quiet.c_lflag &= ~unsigned(ECHO);

                ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);

                std::getline(std::cin, pw);

                ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &was);
            }
            else std::getline(std::cin, pw);

            std::cerr << "\n";
        }
        else std::getline(std::cin, pw);

        if(pw.empty()) {
            std::cerr << "jhttpd: no password given\n";

            return 2;
        }

        try {
            // The user is not asked for: a line is `user:hash` and the caller
            // knows the user.  Printing a bare hash keeps this composable and
            // stops it guessing.
            std::cout << jhttpd::hash_password(pw) << "\n";
        }
        catch(std::exception& e) {
            std::cerr << "jhttpd: " << e.what() << "\n";

            return 1;
        }

        return 0;
    }

    // SIGPIPE from a client that goes away mid-response would kill the process
    // rather than failing the write.  Nothing here wants the default.
    std::signal(SIGPIPE, SIG_IGN);

    // The rotation convention every log tool already knows: move the file
    // aside, send SIGHUP, get a new one.  Without this the process keeps
    // writing to an inode with no name, and the log silently stops existing
    // for anybody looking at the path.
    std::signal(SIGHUP, jhttpd::reopen_on_hup);

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
        p.io_timeout = o.io_timeout;
        p.max_connections = o.max_connections;
        p.max_per_address = o.max_per_address;

        http::server::options so;

        so.max_requests = o.max_requests;

        // The request timeout is policy::io_timeout, not a server option: the
        // async server takes it from the policy at construction and the
        // blocking one hands it to the socket.  One number, two mechanisms,
        // and --io-timeout sets it in the one place it lives.
        so.initial_idle_timeout = o.initial_idle_timeout;
        so.idle_timeout = o.idle_timeout;

        // Read here rather than at the first handshake: a certificate that
        // cannot be read, or a key that does not match it, should stop the
        // program where the operator is watching.
        const sys::tls_context tls =
            o.cert.empty() ? sys::tls_context()
                           : sys::tls_context::server(o.cert, o.key);

        std::unique_ptr<http::server> s;

        if(o.async) {
            s.reset(new http::server(http::server::async_t(), o.port, o.host,
                                     tls, p, so));
        }
        else {
            s.reset(new http::server(o.port, o.host, tls, p, so));
        }

        if(o.rate > 0) s->rate_limit(o.rate, o.burst);

        s->files(pattern_for(o.prefix), o.root, o.cache_control);

        // Held for the life of the server: the verifier below closes over a
        // reference, and a credential file is read once at startup rather than
        // per request.  Re-reading on SIGHUP belongs with the reload work
        // #239 lists.
        std::list<jhttpd::credentials> guards;

        for(std::size_t i = 0; i < o.protect.size(); i++) {
            std::string prefix, realm, file;

            if(!split_protect(o.protect[i], prefix, realm, file)) {
                std::cerr << "jhttpd: --protect wants PREFIX:REALM:FILE, got \""
                          << o.protect[i] << "\"\n";

                return 2;
            }

            guards.push_back(jhttpd::credentials());

            // Throws if the file cannot be read, is world-readable, has a
            // malformed line, or if this build has no libsodium -- and the
            // throw stops the server. A server told to guard a path and
            // silently not guarding it is the worst of the three outcomes.
            guards.back().load(file);

            const jhttpd::credentials& who = guards.back();

            s->protect(pattern_for(prefix), "Basic realm=\"" + realm + "\"",
                       [&who](const http::server::credentials& c) {
                           return who.check(c.user, c.password);
                       });

            std::cerr << "jhttpd: " << pattern_for(prefix) << " needs a "
                      << "credential from " << file << " (" << who.size()
                      << (who.size() == 1 ? " user)" : " users)") << "\n";
        }

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

        // What it is actually doing, on one line, because the next question
        // after "did it start" is always "with what limits".
        std::cerr << "jhttpd: serving " << o.root << " on "
                  << (s->tls() ? "https://" : "http://")
                  << (o.host.empty() ? "*" : o.host) << ":" << s->port()
                  << " under " << pattern_for(o.prefix)
                  << (o.async ? " (async)" : "")
                  << "; rate "
                  << (o.rate > 0 ? std::to_string(o.rate) + "/s burst " +
                                   std::to_string(o.burst)
                                 : std::string("unlimited"))
                  << ", per-address "
                  << (o.max_per_address ? std::to_string(o.max_per_address)
                                        : std::string("unlimited"))
                  << "\n";

        s->run();
    }
    catch(std::exception& e) {
        std::cerr << "jhttpd: " << e.what() << "\n";

        return 1;
    }

    return 0;
}
