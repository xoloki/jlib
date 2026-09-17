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

#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <atomic>
#include <chrono>
#include <vector>

#include <unistd.h>

namespace ai = jlib::ai;
namespace apps = jlib::apps;
namespace http = jlib::net::http;
namespace sys = jlib::sys;
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

    /** Whether try_acquire() should report the model busy. */
    bool busy = false;

    /** Milliseconds per token, so a generation can be abandoned mid-flight. */
    unsigned int per_token_ms = 0;
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

/**
 * Whether a session is out, which the fake has to model for real.
 *
 * `busy` on the script is a switch a test flips; this is exclusion that
 * happens because a generation is running.  Without it the fake cannot show a
 * request holding the model, and a test about abandoning one has nothing to
 * observe -- which is how the first version of that test passed for the wrong
 * reason.
 */
static std::atomic<bool> g_held{false};

struct fake_session {
    fake_template m_templ;
    fake_tokenizer m_tok;

    bool owns = false;

    fake_session() {}

    explicit fake_session(bool own) : owns(own) {}

    fake_session(fake_session&& o) noexcept
        : m_templ(o.m_templ), m_tok(o.m_tok), owns(o.owns) { o.owns = false; }

    ~fake_session() { if(owns) g_held.store(false); }

    fake_session(const fake_session&) = delete;
    fake_session& operator=(const fake_session&) = delete;

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
            if(g_script.per_token_ms)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(g_script.per_token_ms));

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

    /**
     * The check that matters, in the fake as well as the real one.
     *
     * A fake that never blocks cannot deadlock, so without this the *shape* of
     * #273 -- acquiring on the reactor thread -- is invisible here and is
     * found only by pointing a real model at it. juliet's point, and she is
     * right: this is the difference between make check catching it and me
     * catching it at a terminal with eight gigabytes loaded.
     */
    session acquire(const std::string&) {
        if(jlib::sys::on_a_reactor_thread())
            throw std::runtime_error("acquire() blocks, and this is a reactor "
                                     "thread");

        return session(false);
    }

    /**
     * Never blocks, so the reactor may call it.
     *
     * Busy either because a test said so, or because a session really is out
     * -- the second is what an abandoned generation looks like from here.
     */
    std::optional<session> try_acquire(const std::string&) {
        if(g_script.busy) return std::nullopt;

        bool free_now = false;

        if(!g_held.compare_exchange_strong(free_now, true))
            return std::nullopt;

        return session(true);
    }

    /**
     * The other half of not blocking a reactor, and the fake's version of it.
     *
     * The real one loads gigabytes; this only has to prove the endpoint hops
     * somewhere it may block before calling it -- which the check does.
     */
    void prepare(const std::string&) {
        if(jlib::sys::on_a_reactor_thread())
            throw std::runtime_error("prepare() blocks, and this is a reactor "
                                     "thread");

        prepared++;
    }

    std::size_t prepared = 0;
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

/**
 * What was written to stderr while `f` ran.
 *
 * The trim report is a log line and nothing else -- there is no field in a
 * chat completion for "I dropped your history" (#256) -- so this is the only
 * way to assert the thing this branch exists to add.  Without it, deleting the
 * line would break no test.
 */
static std::string captured_stderr(const std::function<void()>& f) {
    const char* path = "app_jserve_test_stderr.txt";

    std::fflush(stderr);

    const int saved = dup(2);

    if(!std::freopen(path, "w", stderr)) { f(); return ""; }

    f();

    std::fflush(stderr);

    dup2(saved, 2);
    close(saved);

    std::ifstream in(path);
    const std::string out((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());

    in.close();
    std::remove(path);

    return out;
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
        // evs.front() on an empty vector is undefined, and this sat inside a
        // try that cannot catch it.  It went unnoticed until a change made the
        // stream empty, at which point the test segfaulted rather than failing.
        if(evs.empty()) {
            ok("  the framing parses", false, "no events at all");

            return;
        }

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

static void a_caller_that_asked_for_little(jlib::net::http::server& s) {
    std::cout << "\na prompt that nearly fills the context:\n";

    // #253.  No trimming happens here -- one turn, and it already fits -- so
    // this is the ordinary shape of it: a file pasted into a harness, and a
    // caller that wants a one-token answer out of what is left.
    //
    // The template spells a turn as "user:CONTENT\n", so the prompt is the
    // content plus six.
    g_script = script();
    g_script.context = 100;
    g_script.tokens.push_back(int('y'));

    const std::string filler(82, 'x');            // prompt 88, room 12

    const reply small = post(s, R"({"model":"m","max_tokens":1,"messages":[{"role":"user","content":")"
                              + filler + R"("}]})");

    ok("a request for one token is answered", small.status == 200,
       std::to_string(small.status) + " " + small.body);

    ok("  and the prompt left twelve free", g_script.saw_prompt.size() == 88,
       std::to_string(g_script.saw_prompt.size()));

    ok("  of which it was given the one it asked for", g_script.saw_budget == 1,
       std::to_string(g_script.saw_budget));

    // The case MIN_REPLY was written for is untouched: asking for more than is
    // there is still refused, and this is the assertion that would fail if the
    // fix had simply deleted the check.
    const reply big = post(s, R"({"model":"m","max_tokens":16,"messages":[{"role":"user","content":")"
                            + filler + R"("}]})");

    ok("a request for sixteen, with twelve free, is still refused",
       big.status == 400, std::to_string(big.status) + " " + big.body);

    try {
        json::object::ptr o = json::object::create(big.body);

        ok("  as context_length_exceeded",
           std::string(o->obj("error")->get("type")) == "context_length_exceeded");
    }
    catch(std::exception& e) { ok("  it unwraps", false, e.what()); }

    // And a request that says nothing about length is judged as it always was,
    // against MIN_REPLY rather than against DEFAULT_RESERVE.
    const reply none = post(s, R"({"model":"m","messages":[{"role":"user","content":")"
                             + filler + R"("}]})");

    ok("a request with no max_tokens is refused on the same twelve",
       none.status == 400, std::to_string(none.status));
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
    // max_tokens is 32 so that trimming is what this case measures.  A
    // smaller one used to be refused outright -- #253, fixed on this branch
    // and covered above.
    std::string body = R"({"model":"m","max_tokens":32,"messages":[)"
        R"({"role":"system","content":"SYS"},)";

    for(int i = 0; i < 12; i++)
        body += R"({"role":"user","content":"turn)" + std::to_string(i) + R"("},)";

    body += R"({"role":"user","content":"LAST"}]})";

    reply a;

    const std::string said = captured_stderr([&]{ a = post(s, body); });

    ok("the request is answered", a.status == 200, std::to_string(a.status) + " " + a.body);

    // The whole of #256: the client sent thirteen turns and got an answer to
    // fewer, and this line is the only place that is written down.
    ok("  and the server says what it trimmed",
       said.find("trimmed") != std::string::npos &&
       said.find("turns to reserve") != std::string::npos,
       said.empty() ? "(nothing on stderr)" : said);

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

static void what_the_trimmer_gave_up() {
    std::cout << "\nwhat the trimmer reports giving up:\n";

    // Called directly rather than over HTTP: the count exists so the endpoint
    // can log it, and a log line is not in the response.  jserve.hh is a
    // header for exactly this reason.
    fake_session s;

    g_script = script();
    g_script.context = 8192;

    std::vector<ai::message> turns;

    // 40 turns whose ids come to 6000, which fits 8192 with room to spare.
    for(int i = 0; i < 40; i++)
        turns.push_back({ "user", std::string(144, char('a' + i % 26)) });

    const std::size_t whole = s.templ().encode(turns, s.tok()).size();

    ok("the conversation fits the context as sent", whole < 8192,
       std::to_string(whole) + " ids");

    {
        const apps::laid_out p = apps::lay_out(s, turns, 0);

        ok("a request with no cap drops nothing", p.dropped == 0,
           std::to_string(p.dropped));

        ok("  and says how many turns it was given", p.turns == 40,
           std::to_string(p.turns));

        ok("  reserving the default", p.reserved == 512, std::to_string(p.reserved));
    }

    {
        // 4096 reserved out of 8192 cannot leave 6000 of prompt, so history
        // goes -- and this is the number nothing used to record (#256).
        const apps::laid_out p = apps::lay_out(s, turns, 4096);

        ok("a generous cap drops turns that would have fitted", p.dropped > 0,
           std::to_string(p.dropped) + " of " + std::to_string(p.turns));

        ok("  reserving what the caller asked for", p.reserved == 4096,
           std::to_string(p.reserved));

        ok("  and leaving room for it", p.ids.size() + 4096 <= 8192,
           std::to_string(p.ids.size()) + " + 4096");

        ok("  with the newest turn still there",
           p.ids.size() > 0 && ids_to_text(p.ids).find(std::string(144, char('a' + 39 % 26)))
           != std::string::npos);
    }

    {
        // The bigger the reservation the more it costs, which is the shape of
        // the table on #256.
        const apps::laid_out four = apps::lay_out(s, turns, 4096);
        const apps::laid_out seven = apps::lay_out(s, turns, 7000);

        ok("and a larger reservation costs more of it",
           seven.dropped > four.dropped,
           std::to_string(four.dropped) + " -> " + std::to_string(seven.dropped));
    }
}

static void a_busy_model_answers_rather_than_waits(jlib::net::http::server& s) {
    std::cout << "\na model that is already busy:\n";

    // #273: acquire() blocks, complete() is a coroutine on the reactor thread,
    // and the reactor is what the holder needs to finish -- so waiting here
    // wedged the server permanently.  Busy is an answer now.
    g_script = script();
    g_script.busy = true;
    g_script.tokens.push_back(int('x'));

    const reply a = post(s, R"({"model":"m","messages":[{"role":"user","content":"hi"}]})");

    ok("it answers 503", a.status == 503, std::to_string(a.status) + " " + a.body);

    try {
        json::object::ptr o = json::object::create(a.body);

        ok("  saying which model, and that it is busy",
           std::string(o->obj("error")->get("message")).find("busy") !=
               std::string::npos &&
           std::string(o->obj("error")->get("type")) == "server_busy", a.body);
    }
    catch(std::exception& e) { ok("  it unwraps", false, e.what()); }

    // And the server is still a server: the next request is served.
    g_script.busy = false;
    g_script.tokens.push_back(int('y'));

    const reply b = post(s, R"({"model":"m","messages":[{"role":"user","content":"hi"}]})");

    ok("and it is still serving afterwards", b.status == 200,
       std::to_string(b.status));
}

/** The request bytes, sent raw so the socket can be closed mid-generation. */
static void abandon(unsigned short port, const std::string& body) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);

    if(fd < 0) return;

    sockaddr_in a{};

    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        std::ostringstream head;

        head << "POST /v1/chat/completions HTTP/1.1\r\n"
             << "Host: 127.0.0.1\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << body.size() << "\r\n\r\n" << body;

        const std::string wire = head.str();

        if(::write(fd, wire.data(), wire.size()) < 0) {}

        // Let it start generating, then hang up without reading a byte.
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }

    ::close(fd);
}

static void a_client_that_hangs_up(jlib::net::http::server& s) {
    std::cout << "\na client that hangs up mid-generation:\n";

    // The non-streaming path writes nothing until the generation is over, so
    // relay::wanted() -- which learns from a failed write -- can never fire on
    // it.  A client that left held the model to the end, and the mechanism
    // written to stop exactly that could not.
    g_script = script();
    g_script.per_token_ms = 60;

    for(int i = 0; i < 40; i++) g_script.tokens.push_back(int('z'));

    const unsigned short port = s.port();

    std::thread gone([port]{
        abandon(port, R"({"model":"m","messages":[{"role":"user","content":"long"}]})");
    });

    // While that one is generating **and still connected**, the model is busy.
    // Checked before the hang-up rather than after: the whole point of the fix
    // is that after it, the model is free -- so a check at the wrong moment
    // asserts the old behaviour and fails against the new.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const reply busy = post(s, R"({"model":"m","messages":[{"role":"user","content":"hi"}]})");

    ok("while it runs, the model is busy", busy.status == 503,
       std::to_string(busy.status));

    gone.join();

    // 40 tokens at 60ms is 2.4 seconds if it runs to the end, and the hang-up
    // is at 800ms.  A generation that noticed is finished within a token or
    // two of that; one that did not is still going for another second and a
    // half, and the request below gets a 503.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    g_script.per_token_ms = 0;

    const reply after = post(s, R"({"model":"m","messages":[{"role":"user","content":"hi"}]})");

    ok("and once it goes, the model is free again", after.status == 200,
       std::to_string(after.status) + " " + after.body);
}

int main() {
    std::cout << std::unitbuf;

    ai::host_backend<T> backend;

    apps::options o;

    o.port = 0;
    o.host = "127.0.0.1";
    o.models.push_back(std::make_pair(std::string("m"), std::string("/nonesuch.gguf")));

    fake_endpoint e(backend, o);

    // **Not the default policy.**  The endpoint hops to the pool to load a
    // model without blocking the reactor, and `job_queue` runs a hop inline
    // when it has no threads -- so zero means "still on the reactor", which is
    // what the hop exists to avoid.  jserve's own default is 1 for the same
    // reason (#273).
    sys::server::policy p;

    p.threads = 2;

    jlib::net::http::server s(jlib::net::http::server::async_t(), 0,
                              "127.0.0.1", jlib::sys::tls_context(), p);

    s.transport().on_error([](const std::exception&, const jlib::sys::peer&) {});

    e.wire(s);

    std::thread t([&s]{ s.run(); });

    what_the_trimmer_gave_up();
    the_models_route(s);
    a_busy_model_answers_rather_than_waits(s);
    a_client_that_hangs_up(s);
    a_whole_reply(s);
    a_streamed_reply(s);
    a_character_that_arrives_in_pieces(s);
    a_reply_cut_mid_character(s);
    a_caller_that_asked_for_little(s);
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
