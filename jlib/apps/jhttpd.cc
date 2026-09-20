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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <jlib/apps/jhttpd.hh>

#include <jlib/net/http_server.hh>
#include <jlib/sys/sys.hh>
#include <jlib/util/conf.hh>

#include <csignal>
#include <list>
#include <termios.h>
#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace jlib;
using namespace jlib::net;

namespace {

using jhttpd::guard;
using jhttpd::options;
using jhttpd::read_config;
using jhttpd::site;

bool split_vhost(const std::string& spec, std::string& name, std::string& root,
                 std::string& cert, std::string& key);
bool split_protect(const std::string& spec, std::string& prefix,
                   std::string& realm, std::string& file);

void usage(std::ostream& o, const char* argv0) {
    o << "usage: " << argv0 << " [options]\n"
      << "\n"
      << "  Serve a directory over HTTP, and write down what happened.\n"
      << "\n"
      << "  --root DIR        what to serve (default .)\n"
      << "  --prefix PATH     the URL prefix it is served under (default /)\n"
      << "  --port N          (default 8080; replaces any listen in the config)\n"
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
      << "  --vhost NAME:ROOT[:CERT:KEY]\n"
      << "                    repeatable; serve ROOT to requests naming NAME,\n"
      << "                    and present CERT to clients that ask for NAME.\n"
      << "                    --root remains the site for every other name,\n"
      << "                    and --cert the certificate for every other name.\n"
      << "\n"
      << "  Authentication:\n"
      << "  --config FILE               an nginx-shaped config; flags override it\n"
      << "  --protect PREFIX:REALM:FILE\n"
      << "                    require a credential under PREFIX.  FILE holds\n"
      << "                    one user:hash line each, Argon2id, mode 600.\n"
      << "  --hash-password   read a password and print a line for that file\n"
      << "\n"
      << "  Running it:\n"
      << "  --user NAME       drop to this user after binding and opening\n"
      << "  --group NAME      and this group (default: the user's own)\n"
      << "  --pidfile FILE    written after binding, before dropping\n"
      << "  --allow-root      run as root anyway, which is refused by default\n"
      << "  --test            check the config and what it names, then exit\n"
      << "  --daemon          fork into the background; the command does not\n"
      << "                    return until the server is bound and serving,\n"
      << "                    and exits non-zero if it never got there\n"
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
        if(a == "--allow-root") { o.allow_root = true; continue; }
        if(a == "--daemon") { o.daemon = true; continue; }
        if(a == "--test") { o.test = true; continue; }

        if(a.size() > 2 && a.compare(0, 2, "--") == 0) {
            if(i + 1 >= argc) {
                std::cerr << "jhttpd: " << a << " wants a value\n";

                return false;
            }

            const std::string v = argv[++i];

            if(a == "--root") o.root = v;
            else if(a == "--prefix") o.prefix = v;
            // **A flag replaces the list rather than editing it.**  That is
            // the only reading that keeps "a flag always overrides the file"
            // true: --port 9000 against a config with two listeners has to
            // mean one port, or the flag would be adding a third.  Both flags
            // land on the same single entry, so --host and --port compose.
            else if(a == "--port") {
                if(o.listens.size() != 1) o.listens.assign(1, jhttpd::listen_spec());

                o.listens[0].port = (unsigned short)std::atoi(v.c_str());
            }
            else if(a == "--host") {
                if(o.listens.size() != 1) o.listens.assign(1, jhttpd::listen_spec());

                o.listens[0].host = v;
            }
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
            else if(a == "--config") o.config = v;
            else if(a == "--protect") {
                // **Split here rather than at the point of use.**  A spec that
                // cannot be read is a mistake in the command line, and the
                // command line is what the operator is looking at right now --
                // reporting it three hundred lines later, after the logs are
                // open and the privileges dropped, tells them the same thing
                // at a worse moment.
                guard g;

                if(!split_protect(v, g.prefix, g.realm, g.file)) {
                    std::cerr << "jhttpd: --protect wants PREFIX:REALM:FILE, "
                              << "got \"" << v << "\"\n";

                    return false;
                }

                o.protect.push_back(g);
            }
            else if(a == "--vhost") {
                site s;

                if(!split_vhost(v, s.name, s.root, s.cert, s.key)) {
                    std::cerr << "jhttpd: --vhost wants NAME:ROOT or "
                              << "NAME:ROOT:CERT:KEY, got \"" << v << "\"\n";

                    return false;
                }

                o.vhosts.push_back(s);
            }
            else if(a == "--user") o.user = v;
            else if(a == "--group") o.group = v;
            else if(a == "--pidfile") o.pidfile = v;
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

    if(!o.group.empty() && o.user.empty()) {
        std::cerr << "jhttpd: --group without --user; there is nothing to "
                  << "drop to\n";

        return false;
    }

    return true;
}

/**
 * `NAME:ROOT` or `NAME:ROOT:CERT:KEY`.
 *
 * Left to right, unlike `--protect`: a DNS name cannot contain a colon, so the
 * first one always ends the name. The optional pair is recognised by there
 * being exactly four fields, which is why a root containing colons is only
 * legal in the two-field form -- a limitation worth stating rather than
 * pretending the grammar is unambiguous.
 */
bool split_vhost(const std::string& spec, std::string& name, std::string& root,
                 std::string& cert, std::string& key)
{
    std::vector<std::string> bits;
    std::string::size_type at = 0;

    for(;;) {
        const std::string::size_type c = spec.find(':', at);

        if(c == std::string::npos) { bits.push_back(spec.substr(at)); break; }

        bits.push_back(spec.substr(at, c - at));

        at = c + 1;
    }

    if(bits.size() == 2) {
        name = bits[0];
        root = bits[1];
        cert.clear();
        key.clear();
    }
    else if(bits.size() == 4) {
        name = bits[0];
        root = bits[1];
        cert = bits[2];
        key = bits[3];
    }
    else return false;

    if(name.empty() || root.empty()) return false;

    return bits.size() == 2 || (!cert.empty() && !key.empty());
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

    // **The config is read before any flag is applied, so a flag overrides
    // it.**  Found by a scan of its own rather than from inside parse(),
    // because parse() is what does the overriding: reading the file as the
    // loop reached it would make the outcome depend on where --config sat
    // among the other flags, so `--port 9 --config f` and `--config f --port
    // 9` would disagree.  They must not.
    for(int i = 1; i + 1 < argc; i++) {
        if(std::string(argv[i]) == "--config") o.config = argv[i + 1];
    }

    if(!o.config.empty()) {
        try {
            read_config(o.config, o);
        }
        catch(const std::exception& e) {
            std::cerr << "jhttpd: " << e.what() << "\n";

            return 2;
        }
    }

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
    // Before any handler, so each always has somewhere to write.  The daemon
    // fork below inherits both ends, which is what is wanted: FD_CLOEXEC
    // closes them on exec, not on fork.
    //
    // A failure here costs the config reload and the graceful stop, and
    // nothing else -- the log files notice the counter rather than the pipe,
    // so rotation still works.
    if(!sys::wakeup::arm()) {
        std::cerr << "jhttpd: no pipe for signals, so the config will not "
                  << "reload and SIGINT will not stop it gracefully; log "
                  << "rotation still works\n";
    }

    std::signal(SIGHUP, sys::wakeup::on_signal);

    // **Stopping on purpose, rather than being killed.**
    //
    // Without these, the only way out is a signal whose default action
    // terminates the process -- which drops every connection mid-response,
    // leaves the pid file behind, and never runs a destructor. The handler
    // only counts and pokes; the thread below does the part that takes locks.
    std::signal(SIGINT, sys::wakeup::on_signal);
    std::signal(SIGTERM, sys::wakeup::on_signal);

    // **Absolute before anything forks or chdirs**, because --daemon moves to
    // "/" and a relative --root would then mean a different directory than the
    // one the operator typed it in.  Done for every path rather than the
    // obvious one, since they are all resolved later than this.
    sys::daemon bg;

    // Nothing said, by flag or by file: one loopback port, as it always was.
    // Filled here rather than defaulted in the struct so that --port can tell
    // "the operator asked for this" from "nobody asked for anything".
    if(o.listens.empty()) o.listens.push_back(jhttpd::listen_spec());

    o.root = sys::absolute_path(o.root);
    o.access_log = sys::absolute_path(o.access_log);
    o.error_log = sys::absolute_path(o.error_log);
    o.pidfile = sys::absolute_path(o.pidfile);
    o.cert = sys::absolute_path(o.cert);
    o.key = sys::absolute_path(o.key);

    for(std::size_t i = 0; i < o.protect.size(); i++)
        o.protect[i].file = sys::absolute_path(o.protect[i].file);

    for(std::size_t i = 0; i < o.vhosts.size(); i++) {
        o.vhosts[i].root = sys::absolute_path(o.vhosts[i].root);

        if(!o.vhosts[i].cert.empty()) {
            o.vhosts[i].cert = sys::absolute_path(o.vhosts[i].cert);
            o.vhosts[i].key  = sys::absolute_path(o.vhosts[i].key);
        }
    }

    // **--test: everything that can be checked without binding a port.**
    //
    // `apachectl configtest` checks syntax. This checks syntax and then the
    // things a config *names*, because a file that parses and points at a root
    // that is not there is the failure an operator actually has -- and the
    // difference between finding it here and finding it after stopping the old
    // server is the difference between a typo and an outage.
    //
    // Not bound, not served: the port is the one thing that cannot be tested
    // without taking it, and taking it is what the running server is doing.
    if(o.test) {
        int wrong = 0;

        const std::vector<jhttpd::site> roots = jhttpd::every_root(o);

        for(std::size_t i = 0; i < roots.size(); i++) {
            struct stat st;

            if(::stat(roots[i].root.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                std::cout << "  ok    " << roots[i].name << " -> "
                          << roots[i].root << "\n";
            }
            else {
                std::cerr << "  FAIL  " << roots[i].name << " -> "
                          << roots[i].root << ": not a directory\n";

                wrong++;
            }
        }

        if(!o.cert.empty()) {
            try {
                sys::tls_context::server(o.cert, o.key);

                std::cout << "  ok    certificate " << o.cert << "\n";
            }
            catch(std::exception& e) {
                std::cerr << "  FAIL  certificate " << o.cert << ": "
                          << e.what() << "\n";

                wrong++;
            }
        }

        for(std::size_t i = 0; i < o.protect.size(); i++) {
            jhttpd::credentials who;

            try {
                who.load(o.protect[i].file);

                std::cout << "  ok    " << o.protect[i].prefix << " needs a "
                          << "credential from " << o.protect[i].file << " ("
                          << who.size() << ")\n";
            }
            catch(std::exception& e) {
                std::cerr << "  FAIL  " << o.protect[i].prefix << ": "
                          << e.what() << "\n";

                wrong++;
            }
        }

        for(std::size_t i = 0; i < o.listens.size(); i++) {
            std::cout << "  ..    would bind "
                      << (o.listens[i].host.empty() ? "*" : o.listens[i].host)
                      << ":" << o.listens[i].port
                      << (o.listens[i].ssl ? " (tls)" : "")
                      << (o.listens[i].redirect ? " (redirect)" : "") << "\n";
        }

        std::cout << (wrong ? "jhttpd: " + std::to_string(wrong) + " problem(s)\n"
                            : std::string("jhttpd: config is usable\n"));

        return wrong ? 1 : 0;
    }

    // **Before any thread exists.**  The log writer and the server's pool are
    // both threads, and fork() keeps only the calling one -- a mutex another
    // thread held would stay locked for the life of the child.  Nothing above
    // this line starts one.
    if(o.daemon) {
        try { bg.start(); }
        catch(std::exception& e) {
            std::cerr << "jhttpd: " << e.what() << "\n";

            return 1;
        }
    }

    // **Resolved before anything is opened or dropped.**  getpwnam may need
    // to reach a directory service, and a name looked up after the drop is a
    // name that fails only on the machines where that service is not a local
    // file.  A typo in --user should also stop the server before it has bound
    // a port or written a pidfile.
    sys::identity who;

    if(!o.user.empty()) {
        try { who = sys::resolve_identity(o.user, o.group); }
        catch(std::exception& e) {
            std::cerr << "jhttpd: " << e.what() << "\n";

            return 1;
        }
    }

    // A server that binds a port anyone can reach and then answers it as root
    // is one mistake away from being a very bad day.  Refused rather than
    // warned about: a warning is read by exactly the people who did not need
    // it.
    if(::geteuid() == 0 && o.user.empty() && !o.allow_root) {
        std::cerr << "jhttpd: refusing to run as root; give --user NAME to "
                  << "drop after binding, or --allow-root if that is really "
                  << "what you want\n";

        return 1;
    }

    jhttpd::logfile   access;
    jhttpd::error_log errors;

    if(o.access_log != "-" && !access.open(o.access_log)) {
        std::cerr << "jhttpd: cannot write the access log \"" << o.access_log
                  << "\"\n";

        return 1;
    }

    // Takes the level as well as the path, and handles "-" itself: an error
    // line is built by one function now, so stderr and a file cannot drift
    // into two different formats the way they had.
    if(!errors.open(o.error_log, o.error_level)) {
        std::cerr << "jhttpd: cannot write the error log \"" << o.error_log
                  << "\"\n";

        return 1;
    }

    const bool access_to_stdout = o.access_log == "-";

    try {
        sys::server::policy p;

        p.threads = o.threads;
        p.io_timeout = o.io_timeout;
        p.max_connections = o.max_connections;
        p.max_per_address = o.max_per_address;

        http::server::options so;

        so.max_requests = o.max_requests;
        so.index = o.index;

        // The request timeout is policy::io_timeout, not a server option: the
        // async server takes it from the policy at construction and the
        // blocking one hands it to the socket.  One number, two mechanisms,
        // and --io-timeout sets it in the one place it lives.
        so.initial_idle_timeout = o.initial_idle_timeout;
        so.idle_timeout = o.idle_timeout;

        // Read here rather than at the first handshake: a certificate that
        // cannot be read, or a key that does not match it, should stop the
        // program where the operator is watching.
        sys::tls_context tls =
            o.cert.empty() ? sys::tls_context()
                           : sys::tls_context::server(o.cert, o.key);

        // Before the server is built, because the context is copied into it.
        // A --vhost with its own pair and no --cert is refused: there would be
        // no default certificate to fall back to for every other name, and a
        // listener with no identity cannot speak TLS at all.
        for(std::size_t i = 0; i < o.vhosts.size(); i++) {
            const site& v = o.vhosts[i];

            if(v.cert.empty()) continue;

            if(tls.empty()) {
                std::cerr << "jhttpd: site \"" << v.name << "\" has a "
                          << "certificate but the server has none; give "
                          << "--cert and --key too\n";

                return 2;
            }

            tls.add_site(v.name, v.cert, v.key);
        }

        // Every port, bound before anything is served.  A listener marked
        // `ssl` gets the context; the rest get an empty one, which is what
        // makes 80-and-443 one process.
        std::vector<sys::server::bound> ports;

        for(std::size_t i = 0; i < o.listens.size(); i++) {
            const jhttpd::listen_spec& b = o.listens[i];

            ports.push_back(sys::server::bound(
                sys::listener(b.port, b.host),
                b.ssl ? tls : sys::tls_context()));
        }

        // The bind happened above, so the port a `listen 0` asked the kernel
        // for is known now and the redirect can name it.
        unsigned short https_port = 0;

        for(std::size_t i = 0; i < o.listens.size(); i++) {
            if(o.listens[i].ssl) { https_port = ports[i].l.port(); break; }
        }

        std::unique_ptr<http::server> s;

        if(o.async) {
            s.reset(new http::server(http::server::async_t(), std::move(ports),
                                     p, so));
        }
        else {
            s.reset(new http::server(std::move(ports), p, so));
        }

        for(std::size_t i = 0; i < o.listens.size(); i++) {
            if(!o.listens[i].redirect) continue;

            // The config walk refused a redirect with no TLS listener, so
            // https_port is set by the time this runs.
            s->redirect_insecure(o.listens[i].port, https_port);
        }

        if(o.rate > 0) s->rate_limit(o.rate, o.burst);

        s->files(pattern_for(o.prefix), o.root, o.cache_control);

        for(std::size_t i = 0; i < o.vhosts.size(); i++) {
            const site& v = o.vhosts[i];

            // Throws if the root cannot be resolved, which stops the server --
            // a vhost that silently serves nothing is worse than one that
            // refuses to start.
            s->site_of(v.name).files(pattern_for(o.prefix), v.root,
                                     o.cache_control);

            std::cerr << "jhttpd: " << v.name << " -> " << v.root
                      << (v.cert.empty() ? "" : " (own certificate)") << "\n";
        }

        // Held for the life of the server: the verifier below closes over a
        // reference, and a credential file is read once at startup rather than
        // per request.  Re-reading on SIGHUP belongs with the reload work
        // #239 lists.
        std::list<jhttpd::credentials> guards;

        for(std::size_t i = 0; i < o.protect.size(); i++) {
            const std::string& prefix = o.protect[i].prefix;
            const std::string& realm  = o.protect[i].realm;
            const std::string& file   = o.protect[i].file;

            // emplace, not push_back: a credentials holds a mutex now, so it
            // is neither copyable nor movable.
            guards.emplace_back();

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

        s->on_request([&access, &errors, access_to_stdout, &o](
                          const http::server::access& a) {
            // std::time() here rather than in the library: when a request was
            // answered is a log's business, and a record that carried a
            // timestamp would be carrying one format's opinion.
            const std::string line = jhttpd::combined(a, std::time(0),
                                                      o.log_format);

            if(access_to_stdout) std::cout << line << "\n" << std::flush;
            else                 access.write(line);

            // **A refused request is an error-log line too** (#312).
            //
            // Apache writes both: the 400 in the access log says what the
            // client was told, and `AH10244: invalid URI path (...)` in the
            // error log says what the server objected to. With only the first,
            // a traversal attempt is indistinguishable from a typo -- both are
            // a 400 with no cause recorded anywhere.
            //
            // The target is quoted beside the reason, in Apache's parenthesis,
            // because the reason alone does not say which request caused it.
            if(!a.reason.empty()) {
                errors.write(jhttpd::level::error, "http",
                             a.peer.empty()
                                 ? std::string()
                                 : a.peer + ":" + std::to_string(a.peer_port),
                             jhttpd::one_line(a.reason) +
                                 " (" + a.target + ")");
            }
        });

        s->transport().on_error([&errors](const std::exception& e,
                                          const sys::peer& from) {
            // Sorted into a module and a level rather than written flat: a
            // failed handshake and a server that cannot read its own key are
            // not the same event, and a log that spells them identically
            // cannot be read by anyone who is not already suspicious.
            const jhttpd::sorted_error what = jhttpd::sort_error(e);

            // Asked before the line is built, because the levels that are
            // filtered out are exactly the ones that arrive in volume.
            if(!errors.says(what.at)) return;

            errors.write(what.at, what.module, jhttpd::client_of(from),
                         e.what());
        });

        // What it is actually doing, on one line, because the next question
        // after "did it start" is always "with what limits".
        // **The order below is the point of this branch**, and every line of
        // it is a way to get privilege dropping wrong if moved:
        //
        //   - the port is bound above, because a port under 1024 needs
        //     privilege and dropping first makes it unbindable;
        //   - the logs, the credential files and the TLS keys are opened
        //     above, because they live where an unprivileged user cannot
        //     read them;
        //   - the pidfile is written here, still privileged, because /var/run
        //     is not writable by the user we are about to become;
        //   - and only then is the privilege given up.
        if(!o.pidfile.empty()) {
            std::ofstream pid(o.pidfile.c_str(),
                              std::ios::out | std::ios::trunc);

            if(!pid) {
                std::cerr << "jhttpd: cannot write the pidfile \""
                          << o.pidfile << "\"\n";

                return 1;
            }

            pid << ::getpid() << "\n";
        }

        if(!o.user.empty()) {
            try { sys::become(who); }
            catch(std::exception& e) {
                std::cerr << "jhttpd: " << e.what() << "\n";

                return 1;
            }

            std::cerr << "jhttpd: running as " << o.user
                      << (o.group.empty() ? "" : ":" + o.group)
                      << " (uid " << who.uid << ", gid " << who.gid << ")\n";
        }

        // **Bound, opened, dropped -- and only now is the caller told.**  A
        // --daemon that exited zero the moment it forked would tell every
        // script that starts it that a server was running before anything had
        // tried to take a port.
        bg.ready();

        // One line per listener, because a server on two ports that printed
        // one would be telling the operator half of what it did.
        for(std::size_t i = 0; i < o.listens.size(); i++) {
            const jhttpd::listen_spec& b = o.listens[i];

            std::cerr << "jhttpd: " << (b.ssl ? "https://" : "http://")
                      << (b.host.empty() ? "*" : b.host) << ":"
                      << s->ports()[i]
                      << (b.redirect
                              ? " -> redirects to https"
                              : " -> " + o.root)
                      << "\n";
        }

        std::cerr << "jhttpd: serving " << o.root
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

        // **The lifecycle, in the error log** (#314).
        //
        // Ten of the nineteen error lines Apache wrote on this server in a day
        // were these: configured, command line, caught a signal. They are how
        // "is it running, which build, what did it bind" gets answered without
        // attaching to the process -- and `daemon;` sends the banner above to
        // /dev/null, which is how a loopback-only bind stayed invisible while
        // six sites were dark.
        //
        // Only when the error log is a file. When it is stderr, the banner has
        // just said all of this in a friendlier shape, and saying it twice
        // down one stream helps nobody.
        if(o.error_log != "-") {
            std::string how = argv[0];

            for(int i = 1; i < argc; i++) how += std::string(" ") + argv[i];

            errors.write(jhttpd::level::notice, "core", "",
                         std::string("jhttpd/") + PACKAGE_VERSION +
                             " configured -- resuming normal operations");

            errors.write(jhttpd::level::notice, "core", "",
                         "command line: '" + how + "'");

            // **One line per listener, and the address as bound.** This is the
            // line whose absence cost six sites a morning: `listen 80` binds
            // loopback, `listen :80` binds everything, and nothing in the log
            // said which had happened.
            for(std::size_t i = 0; i < o.listens.size(); i++) {
                const jhttpd::listen_spec& b = o.listens[i];

                errors.write(jhttpd::level::notice, "core", "",
                             std::string("listening on ") +
                                 (b.host.empty() ? "*" : b.host) + ":" +
                                 std::to_string(s->ports()[i]) +
                                 (b.ssl ? " (ssl)" : "") +
                                 (b.redirect ? " (redirect)" : ""));
            }
        }

        // **The reload thread.**
        //
        // Its own thread, and not the reactor's, because everything a reload
        // does is blocking: reading the config file, reading a certificate off
        // disk, and hashing a decoy password with Argon2id, which is slow by
        // design. On the reactor that would stall every connection at once.
        //
        // It blocks on a pipe the handler writes to, rather than polling a
        // flag: write() is async-signal-safe, so the signal reaches an
        // ordinary thread directly. No interval to pick, no wakeups while
        // nothing is happening, and no latency.
        std::atomic<bool> reloading{true};

        /**
         * Owns the reload thread and joins it however this scope ends.
         *
         * **Not tidiness.** A std::thread destroyed while still joinable calls
         * std::terminate, and `s->run()` below can throw -- which would jump
         * past a bare join() straight to the catch, and take the process down
         * with an abort instead of the error message it was about to print.
         *
         * The order matters too: the flag before the poke, because the thread
         * tests it after the read returns. Poking first lets it wake, see a
         * flag that is still true, and block again on a pipe nobody will
         * write to -- and then the join waits forever.
         */
        struct joining {
            joining(std::atomic<bool>& f, std::thread t)
                : m_flag(f), m_thread(std::move(t)) {}

            ~joining() {
                m_flag.store(false);
                jlib::sys::wakeup::poke();

                if(m_thread.joinable()) m_thread.join();
            }

            std::atomic<bool>& m_flag;
            std::thread        m_thread;
        };

        std::sig_atomic_t stops = sys::wakeup::count(SIGINT) +
                                  sys::wakeup::count(SIGTERM);
        std::sig_atomic_t hups = jhttpd::hups();

        std::thread worker([&] {
            while(reloading.load()) {
                const sys::wakeup::woken w = sys::wakeup::wait();

                // Poked or gone means the guard below is winding this thread
                // up; only a signal is worth looking at the counts for.
                if(w != sys::wakeup::woken::signalled) break;

                // **Asked to stop, so stop.**  This runs on an ordinary
                // thread, which is the whole reason the handler only counted:
                // server::stop() takes the pool's mutex and posts to the
                // reactor, and a handler that did either could deadlock
                // against the thread it interrupted.
                const std::sig_atomic_t asked_to_stop =
                    sys::wakeup::count(SIGINT) + sys::wakeup::count(SIGTERM);

                if(asked_to_stop != stops) {
                    stops = asked_to_stop;

                    std::cerr << "jhttpd: stopping\n";

                    // Named, because "who stopped it" is the first question
                    // after an unexplained restart: systemd sends SIGTERM, a
                    // terminal sends SIGINT, and they mean different things
                    // about who did it.
                    errors.write(jhttpd::level::notice, "core", "",
                                 std::string("caught ") +
                                     (sys::wakeup::count(SIGTERM)
                                          ? "SIGTERM"
                                          : "SIGINT") +
                                     ", shutting down");

                    s->stop();

                    break;
                }

                const std::sig_atomic_t asked_to_reload = jhttpd::hups();

                if(asked_to_reload == hups || o.config.empty()) continue;

                hups = asked_to_reload;

                jhttpd::options fresh;

                // Read into a *default* options, not a copy of the live one,
                // so what comes back is what the file says rather than the
                // file laid over what is already running. The flags are
                // deliberately not reapplied: a flag overrode the file at
                // startup and still does, which is why anything a flag set is
                // compared below and reported as needing a restart.
                try {
                    jhttpd::read_config(o.config, fresh);
                }
                catch(std::exception& e) {
                    // The whole point of reading into `fresh`: a config with a
                    // typo in it leaves the server exactly as it was.
                    std::cerr << "jhttpd: reload refused, keeping what is "
                              << "running: " << e.what() << "\n";

                    // At `error`: a reload that was asked for and refused is
                    // the one lifecycle event an operator has to act on, and
                    // it is invisible from outside -- the server carries on
                    // serving the old config perfectly well.
                    errors.write(jhttpd::level::error, "core", "",
                                 std::string("reload refused, keeping what is "
                                             "running: ") + e.what());

                    continue;
                }

                if(fresh.listens.empty())
                    fresh.listens.push_back(jhttpd::listen_spec());

                fresh.root = sys::absolute_path(fresh.root);
                fresh.cert = sys::absolute_path(fresh.cert);
                fresh.key = sys::absolute_path(fresh.key);

                for(std::size_t i = 0; i < fresh.protect.size(); i++)
                    fresh.protect[i].file = sys::absolute_path(fresh.protect[i].file);

                for(std::size_t i = 0; i < fresh.vhosts.size(); i++)
                    fresh.vhosts[i].root = sys::absolute_path(fresh.vhosts[i].root);

                const std::vector<std::string> stuck =
                    jhttpd::needs_a_restart(o, fresh);

                for(std::size_t i = 0; i < stuck.size(); i++) {
                    std::cerr << "jhttpd: \"" << stuck[i] << "\" changed and "
                              << "needs a restart; still using the old one\n";
                }

                // --- the certificate ---
                //
                // Rebuilt every time rather than when the path changes, which
                // is the case that matters: certbot rewrites the same file
                // every couple of months, so the path is exactly what does
                // *not* change when the certificate does.
                if(!fresh.cert.empty()) {
                    try {
                        sys::tls_context next =
                            sys::tls_context::server(fresh.cert, fresh.key);

                        for(std::size_t i = 0; i < fresh.vhosts.size(); i++) {
                            const jhttpd::site& v = fresh.vhosts[i];

                            if(!v.cert.empty())
                                next.add_site(v.name, v.cert, v.key);
                        }

                        for(std::size_t i = 0; i < o.listens.size(); i++) {
                            if(o.listens[i].ssl) s->transport().reload_tls(i, next);
                        }

                        std::cerr << "jhttpd: certificate reloaded from "
                                  << fresh.cert << "\n";
                    }
                    catch(std::exception& e) {
                        std::cerr << "jhttpd: certificate not reloaded, keeping "
                                  << "the old one: " << e.what() << "\n";
                    }
                }

                // --- the credential files ---
                //
                // Same argument: a user is added by editing the file, not by
                // renaming it, so the path staying the same is the normal case
                // and re-reading unconditionally is what picks the change up.
                // Guards are matched by position, which holds because a change
                // to the list itself was reported as needing a restart above.
                {
                    std::size_t i = 0;

                    for(std::list<jhttpd::credentials>::iterator g = guards.begin();
                        g != guards.end() && i < o.protect.size(); ++g, ++i) {
                        try {
                            g->load(o.protect[i].file);
                        }
                        catch(std::exception& e) {
                            std::cerr << "jhttpd: credentials not reloaded from "
                                      << o.protect[i].file << ", keeping the "
                                      << "old ones: " << e.what() << "\n";
                        }
                    }
                }

                // --- the rate limit ---
                if(fresh.rate != o.rate || fresh.burst != o.burst) {
                    s->rate_limit(fresh.rate, fresh.burst);

                    std::cerr << "jhttpd: rate limit now "
                              << (fresh.rate > 0
                                      ? std::to_string(fresh.rate) + "/s burst " +
                                        std::to_string(fresh.burst)
                                      : std::string("unlimited"))
                              << "\n";

                    o.rate = fresh.rate;
                    o.burst = fresh.burst;
                }

                std::cerr << "jhttpd: reloaded " << o.config << "\n";

                // The other half of the refusal notice above: a log that says
                // only when a reload failed leaves "did my SIGHUP arrive at
                // all" unanswerable, and that is the question a rotation or a
                // certbot hook actually raises.
                errors.write(jhttpd::level::notice, "core", "",
                             "reloaded " + o.config);

                // At `warn`, because this is a reload that half-happened: the
                // operator edited something and the running server did not
                // take it, which looks exactly like a successful reload from
                // outside.
                for(std::size_t i = 0; i < stuck.size(); i++) {
                    errors.write(jhttpd::level::warn, "core", "",
                                 "\"" + stuck[i] + "\" changed and needs a "
                                 "restart; still using the old one");
                }
            }
        });

        const joining reloader(reloading, std::move(worker));

        // Returns when the thread above calls stop(), which is what SIGINT
        // and SIGTERM now reach, or if it throws -- and the guard joins on
        // both paths.
        s->run();
    }
    catch(std::exception& e) {
        std::cerr << "jhttpd: " << e.what() << "\n";

        return 1;
    }

    return 0;
}
