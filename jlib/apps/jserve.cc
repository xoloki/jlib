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
 * jserve -- an OpenAI-compatible endpoint in front of a GGUF model.
 *
 *     jserve --port 8080 tiny=tinyllama-1.1b-chat-v1.0.Q8_0.gguf
 *     OPENAI_API_BASE=http://127.0.0.1:8080/v1 OPENAI_API_KEY=x \
 *         aider --model openai/tiny
 *
 * ## What it is for, and what that rules out
 *
 * A coding harness, and **aider specifically** -- because aider's `whole`
 * edit format asks the model for a filename and a fenced block of the whole
 * file, which is prose.  Every other harness worth naming drives a model
 * through `tool_calls`, which is a larger contract and is not implemented.
 * A request carrying `tools` is refused with a 400 saying so, rather than
 * answered with prose that would look like a model too weak to call them.
 *
 * ## Loopback, and it says so
 *
 * `sys::server` and `net::http::server` both state in their headers that they
 * are not hardened for a public port, and this inherits every word of that.
 * It binds 127.0.0.1 unless told otherwise, ignores the API key entirely, and
 * has no rate limit, no request budget and no authentication.  Pointing it at
 * an interface a stranger can reach is not a configuration choice, it is a
 * different program that has not been written.
 */

#include <jlib/ai/engine.hh>
#include <jlib/ai/generate.hh>
#include <jlib/ai/openai.hh>
#include <jlib/ai/sampler.hh>
#include <jlib/apps/jserve.hh>
#include <jlib/sys/await.hh>
#include <jlib/sys/relay.hh>

#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include <unistd.h>

#ifdef HAVE_METAL
#include <jlib/metal/backend.hh>
#endif

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace ai = jlib::ai;
namespace http = jlib::net::http;
namespace sys = jlib::sys;

using jlib::apps::options;
using jlib::apps::endpoint;

namespace {


std::atomic<bool> stopping(false);

http::server* running = 0;

void on_signal(int) {
    stopping = true;

    if(running) running->stop();
}

void usage(std::ostream& o, const char* argv0) {
    o << "usage: " << argv0 << " [options] <name>=<model.gguf> ...\n"
      << "\n"
      << "  An OpenAI-compatible endpoint, for a coding harness on loopback.\n"
      << "\n"
      << "  --port N        (default 8080)\n"
      << "  --host ADDR     (default 127.0.0.1; see the header before changing)\n"
      << "  --threads N     workers for request handling, 0 to do it inline\n"
      << "  --temp F        when a request does not say (default 0.8)\n"
      << "  --top-k N       (default 40)\n"
      << "  --top-p F       (default 0.95)\n"
      << "  --repeat F      discourage a token already seen; 1 is off\n"
      << "  --seed N\n"
      << "  --help\n";
}

bool parse(int argc, char** argv, options& o) {
    for(int i = 1; i < argc; i++) {
        const std::string a = argv[i];

        if(a == "--help") { usage(std::cout, argv[0]); std::exit(0); }

        if(a.size() > 2 && a.compare(0, 2, "--") == 0) {
            if(i + 1 >= argc) {
                std::cerr << "jserve: " << a << " wants a value\n";

                return false;
            }

            const std::string v = argv[++i];

            if(a == "--port") o.port = (unsigned short)std::atoi(v.c_str());
            else if(a == "--host") o.host = v;
            else if(a == "--threads") o.threads = unsigned(std::atoi(v.c_str()));
            else if(a == "--temp") o.sampling.temperature = float(std::atof(v.c_str()));
            else if(a == "--top-k") o.sampling.top_k = unsigned(std::atoi(v.c_str()));
            else if(a == "--top-p") o.sampling.top_p = float(std::atof(v.c_str()));
            else if(a == "--repeat")
                o.sampling.repetition_penalty = float(std::atof(v.c_str()));
            else if(a == "--seed")
                o.sampling.seed = std::uint64_t(std::atoll(v.c_str()));
            else {
                std::cerr << "jserve: no such option " << a << "\n";

                return false;
            }

            continue;
        }

        // name=path.  A bare path is named after itself, so a caller who does
        // not care can leave the name off.
        const std::string::size_type eq = a.find('=');

        if(eq == std::string::npos) o.models.push_back(std::make_pair(a, a));
        else o.models.push_back(std::make_pair(a.substr(0, eq),
                                               a.substr(eq + 1)));
    }

    if(o.models.empty()) {
        usage(std::cerr, argv[0]);

        return false;
    }

    return true;
}


}

template<typename T>
int run(ai::backend<T>& b, const options& o) {
    endpoint<T> e(b, o);

    sys::server::policy p;

    // **Not connections.**  A connection is a coroutine on the reactor now,
    // not a thread, so this bounds nothing about how many clients can be
    // served at once -- it is the pool that parses requests and runs buffered
    // handlers, and 0 runs them on the reactor's thread.
    //
    // A generation is on neither: it gets its own thread and reports back
    // through a pipe.  See sys::relay.
    p.threads = o.threads;

    // **The default, which this used to have to turn off.**
    //
    // On the blocking server io_timeout is SO_RCVTIMEO and SO_SNDTIMEO, per
    // operation, so a reply produced over seconds could trip it mid-token and
    // the only way out was to disable the bound entirely.  The async server
    // arms a deadline over reading the *request* and cancels it before the
    // handler runs, so a long reply was never in its reach -- and leaving it
    // at 0 now would give up the slow-loris bound for nothing.
    //
    // What bounds a reply instead is nothing, deliberately: a generation takes
    // as long as it takes, and the client asking for it is not idle.

    http::server s(http::server::async_t(), o.port, o.host,
                   sys::tls_context(), p);

    e.wire(s);

    s.transport().on_error([](const std::exception& x, const sys::peer&) {
        if(!stopping) std::cerr << "jserve: " << x.what() << "\n";
    });

    running = &s;

    std::cerr << "jserve: " << s.url("/v1") << "\n";

    for(const std::pair<std::string, std::string>& m : o.models)
        std::cerr << "  " << m.first << " -> " << m.second << "\n";

    std::cerr << "\n  OPENAI_API_BASE=" << s.url("/v1")
              << " OPENAI_API_KEY=x aider --model openai/"
              << o.models.front().first << "\n";

    s.run();

    running = 0;

    return 0;
}

int main(int argc, char** argv) {
    options o;

    if(!parse(argc, argv, o)) return 2;

    // Ignored rather than fatal: a client that has gone leaves a write to a
    // closed socket, and that is a normal end to a stream rather than a
    // reason to die.
    std::signal(SIGPIPE, SIG_IGN);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    try {
#ifdef HAVE_METAL
        std::shared_ptr<jlib::metal::backend<_Float16> > gpu;

        try { gpu.reset(new jlib::metal::backend<_Float16>); }
        catch(std::exception& e) {
            std::cerr << "jserve: no Metal backend (" << e.what() << ")\n";
        }

        if(gpu) return run<_Float16>(*gpu, o);
#endif

        std::cerr << "jserve: running on the CPU in float -- this will be "
                  << "very slow\n";

        ai::host_backend<float> b;

        return run<float>(b, o);
    }
    catch(std::exception& e) {
        std::cerr << "jserve: " << e.what() << "\n";

        return 1;
    }
}
