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

#ifndef JLIB_APPS_JSERVE_HH
#define JLIB_APPS_JSERVE_HH

/**
 * @file
 *
 * jserve's endpoint, in a header so that something other than jserve can hold
 * it -- which for now means a test (#236).
 *
 * It was all in `jserve.cc` and unreachable: the only consumer of
 * `net::http::server` in the tree had no test in `make check`, because driving
 * it meant loading a GGUF, and nothing in CI can reasonably download one.
 * What was left uncovered was not the model -- `ai::generate` has its own
 * tests -- but the **glue**: an OpenAI request becoming turns, the trimming in
 * lay_out(), the system turn folded for a template that refuses one, the order
 * of events in a stream, and every refusal.
 *
 * Two changes make that drivable, and neither is a hook that exists only for
 * the test:
 *
 * - `endpoint` is templated on the **engine**, so a test supplies one whose
 *   session answers with a fixed sequence of tokens.  The default is
 *   `ai::engine<T>` and jserve names nothing.
 * - generation goes through `session::generate()` rather than reaching in for
 *   `model()` and pairing it with a backend by hand -- which is a better call
 *   for jserve anyway, since a session already knows which model it holds.
 *
 * What a fake engine has to provide is what the endpoint uses and no more:
 * `has`, `acquire`, `names`, a constructor taking `ai::backend<T>&`, and a
 * session with `context()`, `templ().encode()`, `tok().eos()`, `tok().piece()`
 * and `generate()`.  It is duck-typed rather than an interface on purpose --
 * an abstract base here would be a virtual call per token on the real path,
 * paid forever for a test.
 */

#include <jlib/ai/engine.hh>
#include <jlib/ai/generate.hh>
#include <jlib/ai/openai.hh>
#include <jlib/ai/sampler.hh>
#include <jlib/net/http_server.hh>
#include <jlib/sys/await.hh>
#include <jlib/sys/relay.hh>
#include <jlib/util/utf8.hh>

#include <algorithm>
#include <ctime>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace jlib {
namespace apps {

namespace ai = jlib::ai;
namespace oa = jlib::ai::openai;
namespace util = jlib::util;
namespace http = jlib::net::http;
namespace sys = jlib::sys;

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

template<typename Session>
laid_out lay_out(Session& s, std::vector<ai::message> turns,
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

    // Enough room for what the caller asked for, or for MIN_REPLY when the
    // caller asked for more than that.
    //
    // `room >= MIN_REPLY` alone was wrong, and the comment that stood here
    // said why while the code did the opposite: a request saying
    // `max_tokens: 5` wants a five-token answer and must get one, and
    // refusing it for being under MIN_REPLY reads a deliberate choice as a
    // context that does not fit.  A prompt that nearly fills the context --
    // a file pasted into a harness -- was refused with twelve tokens free to
    // a caller that wanted one (#253).
    //
    // MIN_REPLY still decides every case it was written for (#169), because a
    // caller that asks for more than is there is still refused: at
    // `max_tokens: 16` with twelve free this is false, as it was before.
    //
    // What is **not** fixed here is the other end of the same line: `want` is
    // the caller's ceiling, so a generous `max_tokens` makes the trimmer drop
    // history to reserve room the reply will never use (#256).
    out.fits = room >= std::min(want, MIN_REPLY);

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
inline std::vector<ai::message> fold_system(const std::vector<ai::message>& in) {
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

template<typename T, typename Engine = ai::engine<T> >
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
        typename Engine::session s = m_engine.acquire(name);

        laid_out plan;

        // A template that refuses.  Caught here, where a 400 is still
        // possible, rather than escaping into the streaming path where
        // nothing can be said.
        try { plan = lay_out(s, req.messages, req.max_tokens); }
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
                try { plan = lay_out(s, folded, req.max_tokens); }
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
            // A byte-fallback vocabulary hands over one byte per token, and an
            // SSE event carries its content inside a JSON string -- so a
            // four-byte character emitted a piece at a time became four JSON
            // texts that do not parse (#249).  Held here, at the producer,
            // rather than in the streaming path: the non-streaming reply is
            // built from the same relay, and it ends mid-character too when a
            // generation stops between byte-fallback tokens.
            util::utf8_stream chars;

            s.generate(m_backend, plan.ids, plan.budget,
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

                const std::string whole = chars.feed(piece);

                if(!whole.empty()) b.emit(whole);

                return true;
            });

            // Whatever is still waiting for continuation bytes is not going to
            // get them.  Dropped, and said: the protocol has no way to spell
            // "this reply stopped mid-character" -- finish_reason is length or
            // stop and neither is this -- so the log is the only place it can
            // be recorded.
            if(const std::size_t partial = chars.end())
                std::cerr << "jserve: dropped " << partial
                          << " byte(s) of an unfinished character at the end "
                          << "of a reply\n";
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

                // Framed outside the try, which exists for a failed *write*.
                // chunk() refuses content that stops mid-character (#249), and
                // inside the try that refusal was indistinguishable from the
                // client hanging up: the stream ended, without its [DONE], and
                // nothing said why.  A caller's bug should not be able to
                // impersonate a disconnect.
                const std::string ev = oa::event(oa::chunk(id, name, now, d));

                try {
                    co_await out.write(ev);
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

    Engine m_engine;
    options m_o;
    std::string m_default;
};

}
}

#endif // JLIB_APPS_JSERVE_HH
