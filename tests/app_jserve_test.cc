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

// jserve's endpoint, driven with no model file.
//
// The only consumer of net::http::server in the tree had no test, because
// driving it meant a GGUF and nothing in CI can download one (#236).  What
// that left uncovered was never the model -- ai::generate has its own tests --
// but the glue: a request becoming turns, the trimming, the system turn folded
// for a template that refuses one, the order of events in a stream, and every
// refusal.
//
// The engine is a template parameter now, so the fake below answers with a
// scripted sequence of token ids and the whole HTTP path runs.  The server is
// the real one, async, on loopback, and the client is the real one: what is
// faked is the model and nothing else.

#include <jlib/apps/jserve.hh>

#include <jlib/ai/backend.hh>
#include <jlib/net/http.hh>
#include <jlib/util/json.hh>

#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace ai = jlib::ai;
namespace apps = jlib::apps;
namespace http = jlib::net::http;
namespace json = jlib::util::json;
namespace util = jlib::util;

typedef float T;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

// ---------------------------------------------------------------- the fake

/** What the next generation will emit, and what the last one was asked. */
struct script {
    std::vector<int> tokens;         ///< emitted in order, then the reply ends
    std::vector<int> saw_prompt;     ///< what lay_out() produced for it
    unsigned int saw_budget = 0;
    bool refuses_system = false;
    unsigned int context = 4096;
};

static script g_script;

/**
 * A vocabulary that is the byte table: id 0..255 is that byte.
 *
 * Deliberately byte-fallback, because that is the shape #249 was about -- a
 * character arriving one token at a time -- and the assertion that it reaches
 * the client whole is the one that had no test at all.
 */
struct fake_tokenizer {
    int eos() const { return 256; }

    std::string piece(int id) const {
        return id >= 0 && id < 256 ? std::string(1, char(id)) : std::string();
    }
};

/** A template that spells each turn as "role:content\n", so ids are countable. */
struct fake_template {
    std::vector<int> encode(const std::vector<ai::message>& turns,
                            const fake_tokenizer&) const
    {
        std::vector<int> ids;

        for(const ai::message& m : turns) {
            if(g_script.refuses_system && m.role == "system")
                throw std::runtime_error("this template has no system role");

            const std::string line = m.role + ":" + m.content + "\n";

            for(char c : line) ids.push_back(int(static_cast<unsigned char>(c)));
        }

        return ids;
    }
};

struct fake_session {
    fake_template m_templ;
    fake_tokenizer m_tok;

    const fake_template& templ() const { return m_templ; }
    const fake_tokenizer& tok() const { return m_tok; }

    unsigned int context() const { return g_script.context; }

    std::vector<int> generate(ai::backend<T>&, const std::vector<int>& prompt,
                              unsigned int max_new, ai::sampler&,
                              const ai::stops& = ai::stops(),
                              std::function<bool(int)> on_token = nullptr)
    {
        g_script.saw_prompt = prompt;
        g_script.saw_budget = max_new;

        std::vector<int> out = prompt;

        for(std::size_t i = 0; i < g_script.tokens.size() && i < max_new; i++) {
            out.push_back(g_script.tokens[i]);

            if(on_token && !on_token(g_script.tokens[i])) break;
        }

        return out;
    }
};

/** Exactly the surface `endpoint` uses, and nothing else. */
struct fake_engine {
    typedef fake_session session;

    std::vector<std::string> m_names;

    explicit fake_engine(ai::backend<T>&) {}

    void add(const std::string& name, const std::string&) {
        m_names.push_back(name);
    }

    bool has(const std::string& name) const {
        for(const std::string& n : m_names) if(n == name) return true;

        return false;
    }

    std::vector<std::string> names() const { return m_names; }

    session acquire(const std::string&) { return session(); }
};

typedef apps::endpoint<T, fake_engine> fake_endpoint;

// ---------------------------------------------------------------- driving

struct reply {
    int status = 0;
    std::string body;
    bool threw = false;
};

static reply post(jlib::net::http::server& s, const std::string& body) {
    reply a;

    try {
        util::http::fields f;

        f.add("Content-Type", "application/json");

        const util::http::Response r =
            http::request("POST", util::URL(s.url("/v1/chat/completions")), f, body);

        a.status = r.status();
        a.body = r.body();
    }
    catch(std::exception&) { a.threw = true; }

    return a;
}

/** The `data:` payloads of an SSE body, in order, [DONE] included. */
static std::vector<std::string> events(const std::string& body) {
    std::vector<std::string> out;

    std::string::size_type at = 0;

    while((at = body.find("data: ", at)) != std::string::npos) {
        at += 6;

        const std::string::size_type end = body.find("\n\n", at);

        out.push_back(body.substr(at, end == std::string::npos
                                      ? std::string::npos : end - at));

        if(end == std::string::npos) break;

        at = end;
    }

    return out;
}

/** The concatenated `delta.content` of a stream. */
static std::string streamed(const std::vector<std::string>& evs, int& bad) {
    std::string text;

    bad = 0;

    for(const std::string& e : evs) {
        if(e == "[DONE]") continue;

        try {
            json::object::ptr o = json::object::create(e);
            json::object::ptr d = o->arr("choices")->obj(0)->obj("delta");

            if(d->has("content")) text += std::string(d->get("content"));
        }
        catch(std::exception&) { bad++; }
    }

    return text;
}

static std::string ids_to_text(const std::vector<int>& ids) {
    std::string s;

    for(int id : ids) if(id >= 0 && id < 256) s += char(id);

    return s;
}

// ---------------------------------------------------------------- the cases

static void a_whole_reply(jlib::net::http::server& s) {
    std::cout << "\na reply that does not stream:\n";

    g_script = script();
    for(char c : std::string("Paris.")) g_script.tokens.push_back(int(c));

    const reply a = post(s, R"({"model":"m","messages":[{"role":"user","content":"capital?"}]})");

    ok("answers 200", a.status == 200, std::to_string(a.status) + " " + a.body);

    try {
        json::object::ptr o = json::object::create(a.body);
        json::object::ptr choice = o->arr("choices")->obj(0);

        ok("  with the text the model produced",
           std::string(choice->obj("message")->get("content")) == "Paris.",
           a.body);

        ok("  and a finish_reason of stop",
           std::string(choice->get("finish_reason")) == "stop");

        ok("  and a usage block that counts both halves",
           unsigned(o->obj("usage")->get("prompt_tokens")) > 0 &&
           unsigned(o->obj("usage")->get("completion_tokens")) == 6,
           a.body);
    }
    catch(std::exception& e) { ok("  the body parses", false, e.what()); }

    ok("the prompt carried the turn as the template laid it out",
       ids_to_text(g_script.saw_prompt) == "user:capital?\n",
       ids_to_text(g_script.saw_prompt));
}

static void a_streamed_reply(jlib::net::http::server& s) {
    std::cout << "\na reply that streams:\n";

    g_script = script();
    for(char c : std::string("Paris.")) g_script.tokens.push_back(int(c));

    const reply a = post(s, R"({"model":"m","stream":true,"messages":[{"role":"user","content":"capital?"}]})");

    ok("answers 200", a.status == 200, std::to_string(a.status));

    const std::vector<std::string> evs = events(a.body);

    ok("  and ends with [DONE]", !evs.empty() && evs.back() == "[DONE]",
       evs.empty() ? "no events" : evs.back());

    int bad = 0;
    const std::string text = streamed(evs, bad);

    ok("  every event is JSON that parses", bad == 0, std::to_string(bad) + " bad");

    ok("  the deltas concatenate to the reply", text == "Paris.", text);

    try {
        json::object::ptr first = json::object::create(evs.front());

        ok("  the first carries the role and no text",
           std::string(first->arr("choices")->obj(0)->obj("delta")->get("role"))
           == "assistant");

        json::object::ptr last = json::object::create(evs[evs.size() - 2]);

        ok("  and the one before [DONE] carries finish_reason",
           std::string(last->arr("choices")->obj(0)->get("finish_reason")) == "stop");
    }
    catch(std::exception& e) { ok("  the framing parses", false, e.what()); }
}

static void a_character_that_arrives_in_pieces(jlib::net::http::server& s) {
    std::cout << "\na character arriving one token per byte:\n";

    // #249: a byte-fallback vocabulary hands over one byte per token, and an
    // SSE event carries its content inside a JSON string.  This is the guard
    // that had no test in make check -- it was verified against a running
    // jserve and a 1.2 GB model, by hand.
    g_script = script();

    for(unsigned char c : { 0xf0, 0x9f, 0x8d, 0x95 })   // U+1F355
        g_script.tokens.push_back(int(c));

    const reply a = post(s, R"({"model":"m","stream":true,"messages":[{"role":"user","content":"pizza"}]})");

    const std::vector<std::string> evs = events(a.body);

    int bad = 0;
    const std::string text = streamed(evs, bad);

    ok("every event is still JSON that parses", bad == 0,
       std::to_string(bad) + " of " + std::to_string(evs.size()) + " bad");

    ok("  and the character arrives whole", text == "\xf0\x9f\x8d\x95",
       std::to_string(text.size()) + " bytes");

    // The non-streaming path is built from the same relay, so it has the same
    // question and a different answer would mean the holding was put in the
    // wrong place.
    const reply w = post(s, R"({"model":"m","messages":[{"role":"user","content":"pizza"}]})");

    try {
        json::object::ptr o = json::object::create(w.body);

        ok("  the whole reply carries it too",
           std::string(o->arr("choices")->obj(0)->obj("message")->get("content"))
           == "\xf0\x9f\x8d\x95");
    }
    catch(std::exception& e) { ok("  the whole reply parses", false, e.what()); }
}

static void a_reply_cut_mid_character(jlib::net::http::server& s) {
    std::cout << "\na reply cut off inside a character:\n";

    g_script = script();

    for(unsigned char c : { 0xf0, 0x9f, 0x8d, 0x95 })
        g_script.tokens.push_back(int(c));

    // Three of the four bytes, which is what max_tokens does to an emoji.
    const reply a = post(s, R"({"model":"m","max_tokens":3,"messages":[{"role":"user","content":"pizza"}]})");

    ok("the reply is still JSON", !a.body.empty() && a.status == 200,
       std::to_string(a.status));

    try {
        json::object::ptr o = json::object::create(a.body);

        ok("  and its content is empty rather than three orphaned bytes",
           std::string(o->arr("choices")->obj(0)->obj("message")->get("content")).empty(),
           a.body);
    }
    catch(std::exception& e) { ok("  it parses", false, e.what()); }
}

static void the_refusals(jlib::net::http::server& s) {
    std::cout << "\nwhat it refuses, and how it says so:\n";

    g_script = script();

    struct { const char* what; const char* body; int status; const char* says; } cases[] = {
        { "a body that is not JSON", "not json at all", 400, "" },
        { "a request with no messages", R"({"model":"m"})", 400, "" },
        { "a model it does not have",
          R"({"model":"nope","messages":[{"role":"user","content":"hi"}]})", 404, "nope" },
        { "a request asking for tools",
          R"({"model":"m","tools":[{"type":"function"}],"messages":[{"role":"user","content":"hi"}]})",
          400, "tool calling" },
    };

    for(const auto& c : cases) {
        const reply a = post(s, c.body);

        ok(std::string("  ") + c.what + " is refused",
           a.status == c.status, std::to_string(a.status) + " " + a.body);

        // The clients look for error.message and show it; a bare string leaves
        // a caller reading "unknown error" and guessing.
        try {
            json::object::ptr o = json::object::create(a.body);
            const std::string m = o->obj("error")->get("message");

            ok("    with an error.message",
               !m.empty() && (!*c.says || m.find(c.says) != std::string::npos), m);
        }
        catch(std::exception& e) { ok("    that unwraps", false, e.what()); }
    }
}

static void a_context_with_no_room(jlib::net::http::server& s) {
    std::cout << "\na conversation that leaves no room for a reply:\n";

    g_script = script();
    g_script.context = 20;             // MIN_REPLY is 16

    const reply a = post(s, R"({"model":"m","messages":[{"role":"user","content":"012345678901234567890123456789"}]})");

    ok("is refused rather than answered", a.status == 400,
       std::to_string(a.status) + " " + a.body);

    try {
        json::object::ptr o = json::object::create(a.body);

        ok("  as context_length_exceeded",
           std::string(o->obj("error")->get("type")) == "context_length_exceeded",
           a.body);
    }
    catch(std::exception& e) { ok("  it unwraps", false, e.what()); }
}

static void the_oldest_turns_are_dropped(jlib::net::http::server& s) {
    std::cout << "\ntrimming, when the conversation outgrows the context:\n";

    g_script = script();
    g_script.context = 120;
    g_script.tokens.push_back(int('x'));

    // Each turn is "user:N...\n", and the conversation is 153 ids against a
    // context of 120, so the oldest have to go -- but never the newest, and
    // never the system turn.
    //
    // max_tokens is 32 rather than something smaller on purpose: below
    // MIN_REPLY the trim loop frees less than fits() then demands, and the
    // request is refused for having no room when the room it asked for is
    // there.  That is #253, and pinning it here would be pinning a bug.
    std::string body = R"({"model":"m","max_tokens":32,"messages":[)"
        R"({"role":"system","content":"SYS"},)";

    for(int i = 0; i < 12; i++)
        body += R"({"role":"user","content":"turn)" + std::to_string(i) + R"("},)";

    body += R"({"role":"user","content":"LAST"}]})";

    const reply a = post(s, body);

    ok("the request is answered", a.status == 200, std::to_string(a.status) + " " + a.body);

    const std::string prompt = ids_to_text(g_script.saw_prompt);

    ok("  the newest turn survives", prompt.find("LAST") != std::string::npos, prompt);

    ok("  the system turn survives", prompt.find("SYS") != std::string::npos, prompt);

    ok("  the oldest turn is gone", prompt.find("turn0") == std::string::npos, prompt);

    ok("  and what is left fits the context",
       g_script.saw_prompt.size() <= g_script.context, std::to_string(g_script.saw_prompt.size()));
}

static void a_template_that_refuses_a_system_turn(jlib::net::http::server& s) {
    std::cout << "\na template with no system role:\n";

    // Gemma 2 rejects one outright, and every coding harness sends one on
    // every turn -- so a refusal here makes the model unusable rather than
    // merely limited.
    g_script = script();
    g_script.refuses_system = true;
    g_script.tokens.push_back(int('k'));

    const reply a = post(s, R"({"model":"m","messages":[)"
                           R"({"role":"system","content":"BE BRIEF"},)"
                           R"({"role":"user","content":"hi"}]})");

    ok("is answered rather than refused", a.status == 200,
       std::to_string(a.status) + " " + a.body);

    const std::string prompt = ids_to_text(g_script.saw_prompt);

    ok("  with the system turn folded into the user turn",
       prompt.find("BE BRIEF\n\nhi") != std::string::npos, prompt);

    ok("  and no system role left in it",
       prompt.find("system:") == std::string::npos, prompt);
}

static void the_models_route(jlib::net::http::server& s) {
    std::cout << "\nthe models it lists:\n";

    try {
        const util::http::Response r = http::get(util::URL(s.url("/v1/models")));

        ok("GET /v1/models answers 200", r.status() == 200);

        json::object::ptr o = json::object::create(r.body());

        ok("  naming the model it was given",
           std::string(o->arr("data")->obj(0)->get("id")) == "m", r.body());
    }
    catch(std::exception& e) { ok("the route answers", false, e.what()); }
}

int main() {
    std::cout << std::unitbuf;

    ai::host_backend<T> backend;

    apps::options o;

    o.port = 0;
    o.host = "127.0.0.1";
    o.models.push_back(std::make_pair(std::string("m"), std::string("/nonesuch.gguf")));

    fake_endpoint e(backend, o);

    jlib::net::http::server s(jlib::net::http::server::async_t(), 0, "127.0.0.1");

    s.transport().on_error([](const std::exception&, const jlib::sys::peer&) {});

    e.wire(s);

    std::thread t([&s]{ s.run(); });

    the_models_route(s);
    a_whole_reply(s);
    a_streamed_reply(s);
    a_character_that_arrives_in_pieces(s);
    a_reply_cut_mid_character(s);
    the_refusals(s);
    a_context_with_no_room(s);
    the_oldest_turns_are_dropped(s);
    a_template_that_refuses_a_system_turn(s);

    s.stop();
    t.join();

    // What a green run does not establish.
    //
    // **Nothing about a model.**  The fake emits token ids from a script, so
    // every number here is the glue's and none of it is inference.  That is
    // the point -- ai::generate, the sampler and the tokenizer have their own
    // tests -- but it means a change that breaks real generation passes here.
    //
    // Not that a real client is happy.  These are jlib's own HTTP client and
    // jlib's own JSON reader on both ends of the format, which a consistently
    // wrong implementation would also satisfy.  Pointing aider at a running
    // jserve is still the only check that settles that, and it is still by
    // hand (#221 is the other half of that story).
    //
    // Not TLS, and not keep-alive across requests: each case above opens its
    // own connection, because net::http has no pool.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
