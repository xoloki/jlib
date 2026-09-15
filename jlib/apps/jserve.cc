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
#include <jlib/net/http_server.hh>
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
namespace oa = jlib::ai::openai;
namespace http = jlib::net::http;
namespace sys = jlib::sys;

namespace {

/** Room kept for a reply when a request does not say; see #172. */
const unsigned int DEFAULT_RESERVE = 512;

/** Below this a reply is not worth starting; #169 says why. */
const unsigned int MIN_REPLY = 16;

struct options {
    unsigned short port = 8080;
    std::string host = "127.0.0.1";
    unsigned int threads = 0;

    ai::sampler::config sampling;

    /** name=path, in the order given; the first is the default. */
    std::vector<std::pair<std::string, std::string> > models;
};

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

/**
 * The prompt, and what is left of the context for a reply.
 *
 * The same arithmetic jalpaca does (#172): `max_tokens` is a floor kept free
 * for the answer, and the oldest turns are dropped until the prompt leaves
 * that much room.  The newest turn is never dropped -- a request whose own
 * question does not fit is refused rather than answered without it.
 */
struct laid_out {
    std::vector<int> ids;
    unsigned int budget = 0;
    bool fits = true;
};

template<typename T>
laid_out lay_out(typename ai::engine<T>::session& s, std::vector<ai::message> turns,
                 unsigned int req_cap)
{
    laid_out out;

    const unsigned int context = s.context();

    // What to keep free while trimming: the caller's cap when it gave one,
    // since asking for a short answer should let more history survive.
    const unsigned int want = req_cap ? req_cap : DEFAULT_RESERVE;

    for(;;) {
        if(!context) break;

        if(s.templ().encode(turns, s.tok()).size() + want <= context) break;

        std::size_t at = turns.size();

        for(std::size_t i = 0; i + 1 < turns.size(); i++)
            if(turns[i].role != "system") { at = i; break; }

        if(at == turns.size()) break;

        turns.erase(turns.begin() + long(at));
    }

    out.ids = s.templ().encode(turns, s.tok());

    // What the context has left, which is the question MIN_REPLY is about.
    const unsigned int room = context
        ? (context > out.ids.size() ? unsigned(context - out.ids.size()) : 0u)
        : DEFAULT_RESERVE;

    // Asked **before** the caller's cap is applied.  A request saying
    // `max_tokens: 5` wants a five-token answer and must get one; refusing it
    // for being under MIN_REPLY would be reading a deliberate choice as a
    // context that does not fit, which is a different thing entirely.
    out.fits = room >= MIN_REPLY;

    out.budget = room;

    // A cap the caller asked for is a cap, not a target.
    if(req_cap && out.budget > req_cap) out.budget = req_cap;

    return out;
}

/**
 * The system turn merged into the first user turn, or nothing.
 *
 * For a template that refuses a system role.  Returns an empty list when
 * there is nothing to fold -- no system turn, or no user turn to fold it
 * into -- so a caller can tell "already tried" from "worth retrying".
 *
 * The two are joined by a blank line, which is what a model reads as a
 * paragraph break and is what the servers that do this use.
 */
std::vector<ai::message> fold_system(const std::vector<ai::message>& in) {
    std::string system;

    for(const ai::message& m : in)
        if(m.role == "system") system += (system.empty() ? "" : "\n\n")
                                       + m.content;

    if(system.empty()) return std::vector<ai::message>();

    std::vector<ai::message> out;
    bool placed = false;

    for(const ai::message& m : in) {
        if(m.role == "system") continue;

        ai::message t = m;

        if(!placed && t.role == "user") {
            t.content = system + "\n\n" + t.content;
            placed = true;
        }

        out.push_back(t);
    }

    // Nowhere to put it: a conversation of nothing but a system turn is not
    // something to guess about.
    if(!placed) return std::vector<ai::message>();

    return out;
}

template<typename T>
class endpoint {
public:
    endpoint(ai::backend<T>& b, const options& o)
        : m_backend(b), m_engine(b), m_o(o)
    {
        for(const std::pair<std::string, std::string>& m : o.models)
            m_engine.add(m.first, m.second);

        m_default = o.models.front().first;
    }

    void wire(http::server& s) {
        s.route("GET", "/v1/models", [this](const http::server::Request&,
                                            http::server::response& r) {
            r.status(200).type("application/json")
             .body(oa::models(m_engine.names(), 0));
        });

        s.route("POST", "/v1/chat/completions",
                [this](const http::server::Request& q,
                       http::server::async_responder& out)
                    -> sys::task<void> { co_await complete(q, out); });
    }

private:
    sys::task<void> refuse(http::server::async_responder& out, int code,
                           const std::string& why, const std::string& type)
    {
        http::server::response r;

        r.status(code).type("application/json").body(oa::error(why, type));

        co_await out.send(r);
    }

    sys::task<void> complete(const http::server::Request& q,
                             http::server::async_responder& out)
    {
        oa::request req;

        // Every refusal happens here, before anything is sent -- which is the
        // only window a streaming response has for one.  See responder.
        //
        // **co_await cannot appear in a catch handler**, which is a language
        // rule and shapes every refusal below that follows a throw: the reason
        // is recorded, the catch ends, and the answer is sent afterwards.
        std::string bad;

        try { req = oa::request::parse(q.body()); }
        catch(std::exception& e) { bad = e.what(); }

        if(!bad.empty())
            co_return co_await refuse(out, 400, bad, "invalid_request_error");

        if(req.wants_tools)
            co_return co_await refuse(out, 400,
                          "this server does not implement tool calling; "
                          "aider's whole edit format needs none",
                          "invalid_request_error");

        const std::string name = req.model.empty() ? m_default : req.model;

        if(!m_engine.has(name))
            co_return co_await refuse(out, 404, "no model called \"" + name + "\"",
                          "invalid_request_error");

        // Blocks until the model is free.  One conversation at a time per
        // model, because the key-value cache is the conversation -- see
        // ai::engine.
        typename ai::engine<T>::session s = m_engine.acquire(name);

        laid_out plan;

        // A template that refuses.  Caught here, where a 400 is still
        // possible, rather than escaping into the streaming path where
        // nothing can be said.
        try { plan = lay_out<T>(s, req.messages, req.max_tokens); }
        catch(std::exception& first) {
            // Gemma 2 rejects a system role outright, and every coding
            // harness sends one on every turn -- so a refusal here makes the
            // model unusable rather than merely limited.
            //
            // Folded into the first user turn, which is what the other
            // OpenAI-compatible servers do for Gemma and what its own
            // documentation suggests.  **Only after the template has said
            // no**: a model that accepts a system role gets the turn it was
            // sent, unchanged, and this never runs.
            std::vector<ai::message> folded = fold_system(req.messages);

            if(folded.empty()) {
                bad = first.what();
            }
            else {
                try { plan = lay_out<T>(s, folded, req.max_tokens); }
                catch(std::exception& again) { bad = again.what(); }
            }
        }

        if(!bad.empty())
            co_return co_await refuse(out, 400, bad, "invalid_request_error");

        if(!plan.fits)
            co_return co_await refuse(out, 400,
                          "the conversation leaves no room for a reply in a "
                          "context of " + std::to_string(s.context()),
                          "context_length_exceeded");

        ai::sampler::config sc = m_o.sampling;

        if(req.has_temperature) sc.temperature = req.temperature;

        ai::sampler sampler(sc);

        ai::stops ends(s.tok().eos());

        const std::string id = oa::new_id();
        const std::int64_t now = std::int64_t(std::time(0));

        if(req.stream)
            co_await stream(out, s, plan, sampler, ends, req, id, now, name);
        else
            co_await whole(out, s, plan, sampler, ends, req, id, now, name);
    }

    /**
     * Start a generation on its own thread.
     *
     * Everything captured is either owned by the caller's coroutine frame --
     * which outlives this, because sys::relay::join() is in its destructor
     * -- or by the engine session the caller holds.
     */
    template<typename Session>
    void launch(sys::relay<std::string>& ts, Session& s, const laid_out& plan,
                ai::sampler& sampler, const ai::stops& ends,
                unsigned int& made)
    {
        ts.start([this, &s, &plan, &sampler, &ends, &made]
                 (sys::relay<std::string>& b) {
            ai::generate<T>(s.model(), m_backend, plan.ids, plan.budget,
                            sampler, ends, [&](int token) {
                // Asked between tokens, and the only thing that travels this
                // way: a client that has gone should not hold the model for
                // the whole length of a reply nobody will read.
                if(!b.wanted()) return false;

                made++;

                // Empty pieces are real -- some tokens decode to nothing --
                // and emitting one would wake the reader for no text.  Dropped
                // here rather than in the relay, which has no opinion about
                // what an item means.
                const std::string piece = s.tok().piece(token);

                if(!piece.empty()) b.emit(piece);

                return true;
            });
        });
    }

    template<typename Session>
    sys::task<void> stream(http::server::async_responder& out, Session& s,
                           const laid_out& plan, ai::sampler& sampler,
                           const ai::stops& ends, const oa::request& req,
                           const std::string& id, std::int64_t now,
                           const std::string& name)
    {
        http::server::response head;

        // No caching and no proxy buffering: a stream that a proxy holds
        // until it completes is a stream that does not stream.
        head.status(200).type("text/event-stream")
            .field("Cache-Control", "no-cache")
            .field("X-Accel-Buffering", "no");

        co_await out.begin(head);

        {
            oa::delta d;

            d.role = true;

            co_await out.write(oa::event(oa::chunk(id, name, now, d)));
        }

        unsigned int made = 0;

        sys::relay<std::string> ts;

        launch(ts, s, plan, sampler, ends, made);

        // A write that throws is the client having gone.  There is no live()
        // to ask here and there does not need to be: the answer arrives as a
        // failed write, which is the same thing one layer earlier.
        const bool whole_reply = co_await sys::pump(
            ts, out.reactor(),
            [&](const std::string& piece) -> sys::task<bool> {
                oa::delta d;

                d.content = piece;

                try {
                    co_await out.write(oa::event(oa::chunk(id, name, now, d)));
                }
                catch(std::exception&) {
                    // Stops the generation as well, through the bridge -- and
                    // the destructor would anyway, but not until the reply had
                    // been produced in full for nobody.
                    ts.stop();

                    co_return false;
                }

                co_return true;
            });

        ts.join();

        // Nobody to tell.  Returning without the terminating chunk is also
        // right: the response is not finished and should not claim to be.
        if(!whole_reply) co_return;

        const std::string why = ts.why();

        // The generation itself failed.  The 200 left long ago, so the only
        // thing left to say is to stop without terminating -- which is what
        // http::server does with a streaming handler that throws, and this is
        // the same thing said deliberately.
        if(!why.empty()) throw std::runtime_error(why);

        oa::delta last;

        last.done = true;
        last.why = made >= plan.budget ? oa::finish::length : oa::finish::stop;

        co_await out.write(oa::event(oa::chunk(id, name, now, last)));
        co_await out.write(oa::done());
    }

    template<typename Session>
    sys::task<void> whole(http::server::async_responder& out, Session& s,
                          const laid_out& plan, ai::sampler& sampler,
                          const ai::stops& ends, const oa::request& req,
                          const std::string& id, std::int64_t now,
                          const std::string& name)
    {
        std::string body;

        unsigned int made = 0;

        sys::relay<std::string> ts;

        launch(ts, s, plan, sampler, ends, made);

        // The same pump, collecting instead of writing.  Nothing has been sent
        // yet, so a failure here can still be answered -- which is the whole
        // difference between this path and the streaming one.
        co_await sys::pump(ts, out.reactor(),
                      [&body](const std::string& piece) -> sys::task<bool> {
                          body += piece;

                          co_return true;
                      });

        ts.join();

        const std::string why = ts.why();

        if(!why.empty()) {
            co_await refuse(out, 500, why, "internal_error");

            co_return;
        }

        // The leading space the marker convention adds belongs to the prompt,
        // not to the reply; decode() strips it and piece() does not.
        if(!body.empty() && body[0] == ' ') body.erase(0, 1);

        http::server::response r;

        r.status(200).type("application/json")
         .body(oa::completion(id, name, now, body,
                              made >= plan.budget ? oa::finish::length
                                                  : oa::finish::stop,
                              unsigned(plan.ids.size()), made));

        co_await out.send(r);
    }

    // Declared before the engine, which is handed the same reference.
    ai::backend<T>& m_backend;

    ai::engine<T> m_engine;
    options m_o;
    std::string m_default;
};

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
