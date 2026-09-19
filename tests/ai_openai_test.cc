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
 *
 */

#include <jlib/ai/openai.hh>
#include <jlib/util/json.hh>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace ai = jlib::ai;
namespace oa = jlib::ai::openai;
namespace json = jlib::util::json;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** A body of the shape aider sends. */
static void it_reads_a_request() {
    std::cout << "\nit reads a request:\n";

    const oa::request r = oa::request::parse(
        "{\"model\":\"tinyllama\","
        " \"messages\":[{\"role\":\"system\",\"content\":\"Act as an expert.\"},"
        "              {\"role\":\"user\",\"content\":\"Change foo.py\"}],"
        " \"stream\":true, \"temperature\":0.2, \"max_tokens\":900}");

    ok("  the model", r.model == "tinyllama", r.model);

    ok("  both turns, in order",
       r.messages.size() == 2 && r.messages[0].role == "system" &&
       r.messages[1].role == "user" &&
       r.messages[1].content == "Change foo.py",
       std::to_string(r.messages.size()));

    ok("  the streaming flag", r.stream);

    // Absent and zero are different: zero is greedy, which a caller may want.
    ok("  a temperature that was given",
       r.has_temperature && r.temperature > 0.19f && r.temperature < 0.21f,
       std::to_string(r.temperature));

    ok("  and a cap on the reply", r.max_tokens == 900,
       std::to_string(r.max_tokens));

    ok("  no tools were asked for", !r.wants_tools);
}

/** What is absent is not what is zero. */
static void absent_is_not_zero() {
    std::cout << "\nabsent is not zero:\n";

    const oa::request r = oa::request::parse(
        "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");

    ok("  no temperature was given", !r.has_temperature);

    ok("  no cap was given", r.max_tokens == 0);

    ok("  and streaming defaults to off", !r.stream);

    const oa::request greedy = oa::request::parse(
        "{\"temperature\":0,"
        " \"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");

    ok("  where a temperature of zero was given and is not absent",
       greedy.has_temperature && greedy.temperature == 0);
}

/** The newer clients send content as parts rather than a string. */
static void content_may_be_parts() {
    std::cout << "\ncontent may be parts:\n";

    const oa::request r = oa::request::parse(
        "{\"messages\":[{\"role\":\"user\",\"content\":["
        "  {\"type\":\"text\",\"text\":\"first \"},"
        "  {\"type\":\"image_url\",\"image_url\":{\"url\":\"x\"}},"
        "  {\"type\":\"text\",\"text\":\"second\"}]}]}");

    ok("  the text parts are joined",
       r.messages.size() == 1 && r.messages[0].content == "first second",
       r.messages.empty() ? "" : r.messages[0].content);

    ok("  and what is not text is dropped rather than refused",
       r.messages[0].content.find("image") == std::string::npos);
}

/** A tool request reaches the caller whole, in either spelling. */
static void a_tool_request_is_visible() {
    std::cout << "\na tool request is visible:\n";

    const oa::request r = oa::request::parse(
        "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
        " \"tools\":[{\"type\":\"function\"}]}");

    // Answering this with prose looks like a model too weak to call tools,
    // which is far harder to diagnose than a refusal.
    ok("  the caller can tell it was asked", r.wants_tools);

    const oa::request old = oa::request::parse(
        "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
        " \"functions\":[{\"name\":\"f\"}]}");

    ok("  including by the older spelling", old.wants_tools);

    // The definitions themselves, kept as text: a tool's `parameters` is a
    // JSON Schema and there is no struct that holds one without losing
    // something.  #311.
    const oa::request full = oa::request::parse(
        "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
        " \"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"read_file\","
        " \"parameters\":{\"type\":\"object\",\"properties\":"
        "{\"path\":{\"type\":\"string\"}}}}}]}");

    ok("  the schema survives, nested and entire",
       full.tools.find("\"properties\"") != std::string::npos &&
       full.tools.find("\"path\"") != std::string::npos);

    // The older spelling is a bare function list; it is wrapped so a template
    // sees one shape whichever arrived.
    ok("  and the older spelling is wrapped into the newer one's shape",
       old.tools.find("\"type\"") != std::string::npos &&
       old.tools.find("\"function\"") != std::string::npos &&
       old.tools.find("\"name\"") != std::string::npos);

    // Wrapping used to move the parsed function into a new array, which
    // borrowed it from the request and then freed it twice.  Reading it after
    // the request is gone is what that would have shown up as.
    std::string kept;

    {
        const oa::request tmp = oa::request::parse(
            "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
            " \"functions\":[{\"name\":\"f\",\"description\":\"d\"}]}");

        kept = tmp.tools;
    }

    ok("  and outlives the request it came from",
       kept.find("\"f\"") != std::string::npos);

    // tool_choice is carried rather than obeyed, in both shapes the protocol
    // allows -- a server that ignores it answers "required" with prose.
    const oa::request chose = oa::request::parse(
        "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
        " \"tools\":[{\"type\":\"function\"}], \"tool_choice\":\"required\"}");

    ok("  tool_choice arrives as a string", chose.tool_choice == "required");

    const oa::request named = oa::request::parse(
        "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
        " \"tools\":[{\"type\":\"function\"}],"
        " \"tool_choice\":{\"type\":\"function\",\"function\":{\"name\":\"f\"}}}");

    ok("  and as an object naming one",
       named.tool_choice.find("\"f\"") != std::string::npos);

    // Nothing asked for, nothing carried.
    const oa::request none = oa::request::parse(
        "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");

    ok("  and a request with none carries none",
       !none.wants_tools && none.tools.empty() && none.tool_choice.empty());
}

/**
 * A model emits text, not JSON: reading the calls back out of it.
 *
 * #311. Everything here is about what happens when the markup is *not* what
 * the template asked for, because that is what a model under a temperature
 * above zero will occasionally produce -- and because the alternative to
 * noticing is a reply with a sentence silently missing.
 *
 * The convention is Qwen's and ChatML's. A model whose template asks for
 * something else -- Llama's `<|python_tag|>`, Mistral's `[TOOL_CALLS]` --
 * produces text this does not recognise, which comes back as content rather
 * than as a mangled call.
 */
static void calls_are_read_out_of_the_text() {
    std::cout << "\ncalls are read out of the text:\n";

    std::string left;

    ok("  prose is prose",
       oa::calls_in("just text", left).empty() && left == "just text");

    const std::vector<oa::call> one = oa::calls_in(
        "<tool_call>\n{\"name\":\"read_file\",\"arguments\":{\"path\":\"a.c\"}}\n"
        "</tool_call>", left);

    ok("  a call is read, and its arguments stay JSON",
       one.size() == 1 && one[0].name == "read_file" &&
       one[0].arguments.find("a.c") != std::string::npos);

    ok("  and the block is not left in the content", left.empty());

    const std::vector<oa::call> after = oa::calls_in(
        "Sure.<tool_call>{\"name\":\"f\",\"arguments\":{}}</tool_call>", left);

    ok("  a sentence before the call is kept as content",
       after.size() == 1 && left == "Sure.");

    ok("  two calls are two calls",
       oa::calls_in("<tool_call>{\"name\":\"a\",\"arguments\":{}}</tool_call>"
                    "<tool_call>{\"name\":\"b\",\"arguments\":{}}</tool_call>",
                    left).size() == 2);

    // The three ways it can be wrong, and none of them lose text.  A reply
    // cut short mid-call is what a client needs in order to know it was cut
    // short, and a block this cannot read is what the model actually said.
    ok("  an unterminated block stays as content",
       oa::calls_in("<tool_call>{\"name\":\"f\"", left).empty() &&
       left == "<tool_call>{\"name\":\"f\"");

    ok("  a block that is not JSON stays as content",
       oa::calls_in("<tool_call>nope</tool_call>", left).empty() &&
       left == "<tool_call>nope</tool_call>");

    ok("  and so does one with no name",
       oa::calls_in("<tool_call>{\"arguments\":{}}</tool_call>", left).empty() &&
       left == "<tool_call>{\"arguments\":{}}</tool_call>");

    // finish_reason round-trips, because a client that reads tool_calls as
    // stop shows the user an empty turn.
    ok("  tool_calls is a finish reason of its own",
       std::string(oa::spell(oa::finish::tool_calls)) == "tool_calls");

    const oa::answer a = oa::answer::parse(
        "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":"
        "{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"c1\","
        "\"type\":\"function\",\"function\":{\"name\":\"read_file\","
        "\"arguments\":\"{\\\"path\\\":\\\"a.c\\\"}\"}}]}}]}");

    ok("  and an answer carrying one parses",
       a.why == oa::finish::tool_calls && a.calls.size() == 1 &&
       a.calls[0].name == "read_file" && a.calls[0].id == "c1" &&
       a.calls[0].arguments.find("a.c") != std::string::npos);
}

static void what_it_refuses() {
    std::cout << "\nwhat it refuses:\n";

    bool threw = false;

    try { oa::request::parse("{\"model\":\"x\"}"); }
    catch(json::exception&) { threw = true; }

    ok("  a request with no messages", threw);

    threw = false;

    try { oa::request::parse("not json at all"); }
    catch(json::exception&) { threw = true; }

    ok("  and a body that is not JSON", threw);
}

/** The whole-response shape, read back as JSON rather than as a string. */
static void it_writes_a_completion() {
    std::cout << "\nit writes a completion:\n";

    const std::string s = oa::completion("chatcmpl-1", "tinyllama", 1750000000,
                                         "Paris.", oa::finish::stop, 12, 3);

    json::object::ptr o = json::object::create(s);

    ok("  it is an object", bool(o));

    ok("  of the right kind",
       std::string(o->get("object")) == "chat.completion",
       o ? std::string(o->get("object")) : "");

    ok("  carrying the id and model",
       std::string(o->get("id")) == "chatcmpl-1" &&
       std::string(o->get("model")) == "tinyllama");

    json::array::ptr choices = o->arr("choices");

    ok("  with exactly one choice", choices->size() == 1,
       std::to_string(choices->size()));

    json::object::ptr choice = choices->obj(0);

    ok("  whose message is the assistant's",
       std::string(choice->obj("message")->get("role")) == "assistant" &&
       std::string(choice->obj("message")->get("content")) == "Paris.");

    ok("  and which says why it stopped",
       std::string(choice->get("finish_reason")) == "stop");

    json::object::ptr usage = o->obj("usage");

    ok("  the usage adds up",
       int(usage->get("prompt_tokens")) == 12 &&
       int(usage->get("completion_tokens")) == 3 &&
       int(usage->get("total_tokens")) == 15);
}

/** The three kinds of chunk a stream is made of. */
static void it_writes_chunks() {
    std::cout << "\nit writes chunks:\n";

    {
        oa::delta d;

        d.role = true;

        json::object::ptr o = json::object::create(
            oa::chunk("id", "m", 1, d));

        ok("  the first carries the role",
           std::string(o->arr("choices")->obj(0)->obj("delta")->get("role"))
           == "assistant");

        ok("  and is a chunk, not a completion",
           std::string(o->get("object")) == "chat.completion.chunk");
    }

    {
        oa::delta d;

        d.content = "Par";

        json::object::ptr o = json::object::create(oa::chunk("id", "m", 1, d));
        json::object::ptr choice = o->arr("choices")->obj(0);

        ok("  a middle chunk carries content",
           std::string(choice->obj("delta")->get("content")) == "Par");

        // Absent, not null: json-c stores a JSON null as no value at all, and
        // the clients read an absent finish_reason as "not finished".
        ok("  and no finish_reason while it is unfinished",
           !choice->has("finish_reason"));
    }

    {
        oa::delta d;

        d.done = true;
        d.why = oa::finish::length;

        json::object::ptr o = json::object::create(oa::chunk("id", "m", 1, d));
        json::object::ptr choice = o->arr("choices")->obj(0);

        ok("  the last says why it stopped",
           std::string(choice->get("finish_reason")) == "length");

        // Or a client appends an empty string to the reply it has built.
        ok("  and carries no content at all",
           !choice->obj("delta")->has("content"));
    }
}

/** The framing, which is the part that fails silently when it is wrong. */
static void it_frames_events() {
    std::cout << "\nit frames events:\n";

    ok("  an event ends in a blank line",
       oa::event("{}") == "data: {}\n\n", "\"" + oa::event("{}") + "\"");

    ok("  and the terminator is the literal the clients look for",
       oa::done() == "data: [DONE]\n\n");
}

static void it_lists_models() {
    std::cout << "\nit lists models:\n";

    json::object::ptr o = json::object::create(
        oa::models(std::vector<std::string>({ "tinyllama", "gemma" }), 7));

    ok("  as a list", std::string(o->get("object")) == "list");

    json::array::ptr data = o->arr("data");

    ok("  with one entry per model", data->size() == 2,
       std::to_string(data->size()));

    ok("  named as they were given",
       std::string(data->obj(0)->get("id")) == "tinyllama" &&
       std::string(data->obj(1)->get("id")) == "gemma");
}

static void ids_are_unique() {
    std::cout << "\nids are unique:\n";

    ok("  two calls do not collide", oa::new_id() != oa::new_id());
}

static void errors_are_unwrappable() {
    std::cout << "\nerrors are unwrappable:\n";

    json::object::ptr o = json::object::create(
        oa::error("no model called \"x\"", "invalid_request_error"));

    // The clients look for error.message and show it; a bare string leaves a
    // caller reading "unknown error" and guessing.
    ok("  the message is where a client looks for it",
       std::string(o->obj("error")->get("message")) == "no model called \"x\"");

    ok("  and so is the type",
       std::string(o->obj("error")->get("type")) == "invalid_request_error");
}

/** U+1F355, which a byte-fallback vocabulary hands over one byte at a time. */
static const std::string PIZZA = "\xf0\x9f\x8d\x95";

static void a_partial_character_is_refused() {
    std::cout << "\na fragment that is not a whole string:\n";

    // The shape of #249: jserve sent one event per token and the vocabulary
    // gave one byte per token, so this is what reached chunk() four times.
    for(std::size_t n = 1; n < 4; n++) {
        oa::delta d;

        d.content = PIZZA.substr(0, n);

        bool threw = false;

        try { oa::chunk("id", "m", 1, d); }
        catch(json::exception&) { threw = true; }

        ok("  " + std::to_string(n) + " byte(s) of a character is refused", threw);
    }

    {
        oa::delta d;

        d.content = PIZZA;

        bool threw = false;

        try { oa::chunk("id", "m", 1, d); }
        catch(json::exception&) { threw = true; }

        ok("  and the whole character is not", !threw);
    }

    // The tail is the only thing that decides it: a character in the middle of
    // a longer fragment was never the question.
    {
        oa::delta d;

        d.content = "a " + PIZZA + " b";

        bool threw = false;

        try { oa::chunk("id", "m", 1, d); }
        catch(json::exception&) { threw = true; }

        ok("  a character inside a longer fragment is fine", !threw);
    }

    {
        oa::delta d;

        d.content = "hi " + PIZZA.substr(0, 2);

        bool threw = false;

        try { oa::chunk("id", "m", 1, d); }
        catch(json::exception&) { threw = true; }

        ok("  but good text in front of a broken tail does not save it", threw);
    }

    // A whole reply ends mid-character too, when the generation stops between
    // two byte-fallback tokens -- so the non-streaming path needs the same
    // refusal and it is easy to give it only to the streaming one.
    {
        bool threw = false;

        try {
            oa::completion("id", "m", 1, "the answer is " + PIZZA.substr(0, 3),
                           oa::finish::length, 3, 4);
        }
        catch(json::exception&) { threw = true; }

        ok("  and a completion is refused on the same grounds", threw);
    }

    // What the refusal must not do is reject text that is merely not ASCII.
    {
        bool threw = false;

        try {
            oa::completion("id", "m", 1, "caf\xc3\xa9 " + PIZZA,
                           oa::finish::stop, 3, 4);
        }
        catch(json::exception&) { threw = true; }

        ok("  while whole multi-byte text goes through untouched", !threw);
    }
}

static void a_request_survives_the_round_trip() {
    std::cout << "\na request written and read back:\n";

    // The reason the client half belongs in this file: neither direction can
    // check itself, and together they can.  A field that str() forgets or
    // parse() misreads shows up here as a difference and nowhere else.
    oa::request r;

    r.model = "tiny";
    r.messages.push_back({ "system", "be brief" });
    r.messages.push_back({ "user", "capital of France?" });
    r.stream = true;
    r.has_temperature = true;
    r.temperature = 0.25f;
    r.max_tokens = 64;
    r.stop.push_back("\n\n");
    r.stop.push_back("END");

    const oa::request back = oa::request::parse(r.str());

    ok("the model comes back", back.model == r.model, back.model);

    ok("  and both turns, in order",
       back.messages.size() == 2 &&
       back.messages[0].role == "system" && back.messages[0].content == "be brief" &&
       back.messages[1].role == "user" && back.messages[1].content == "capital of France?",
       std::to_string(back.messages.size()) + " turns");

    // Asserted on the **bytes**, not on the round trip.  json-c reads an int
    // 1 as true, so parse() accepts either and back.stream cannot tell them
    // apart -- while a server with a schema, or a client library that checks
    // types, can.  Writing `"stream": 1` passed this test until the
    // assertion stopped going through jlib's own parser to answer it.
    ok("  stream is written as a JSON boolean, not a 1",
       r.str().find("\"stream\": true") != std::string::npos, r.str());

    ok("  and reads back as asked for", back.stream);

    ok("  the temperature that was set",
       back.has_temperature && std::fabs(back.temperature - 0.25f) < 1e-6,
       std::to_string(back.temperature));

    ok("  the cap", back.max_tokens == 64, std::to_string(back.max_tokens));

    // This is the one the round trip earned.  parse() asked str_or() first,
    // which on an array returns the array's own serialised text -- so a list
    // of stop strings arrived as one string spelled `[ "\n\n", "END" ]`, the
    // array branch below it was unreachable, and a client's stop sequences
    // were silently ignored.  Nothing had ever written a request to notice.
    ok("  and both stop strings, as strings",
       back.stop.size() == 2 && back.stop[0] == "\n\n" && back.stop[1] == "END",
       std::to_string(back.stop.size()) + ": " +
       (back.stop.empty() ? std::string() : back.stop[0]));

    // The protocol allows a bare string too, and that path still works.
    const oa::request one = oa::request::parse(
        R"({"model":"m","messages":[{"role":"user","content":"x"}],"stop":"END"})");

    ok("  a bare string is still one stop string",
       one.stop.size() == 1 && one.stop[0] == "END",
       std::to_string(one.stop.size()));
}

static void absent_stays_absent() {
    std::cout << "\nwhat a bare request does not say:\n";

    // Writing the struct's defaults would be asking for greedy sampling and a
    // zero-token reply, neither of which the caller said anything about.
    oa::request r;

    r.model = "tiny";
    r.messages.push_back({ "user", "hi" });

    const std::string body = r.str();

    ok("no temperature", body.find("temperature") == std::string::npos, body);
    ok("no max_tokens", body.find("max_tokens") == std::string::npos, body);
    ok("no stop", body.find("stop") == std::string::npos, body);
    ok("no stream", body.find("stream") == std::string::npos, body);

    const oa::request back = oa::request::parse(body);

    ok("and it reads back as nothing asked for",
       !back.has_temperature && back.max_tokens == 0 &&
       back.stop.empty() && !back.stream);

    // A temperature of zero is a request for greedy, and it has to survive.
    oa::request greedy = r;

    greedy.has_temperature = true;
    greedy.temperature = 0;

    const oa::request gback = oa::request::parse(greedy.str());

    ok("a temperature of zero is not the same as no temperature",
       gback.has_temperature && gback.temperature == 0, greedy.str());
}

static void a_completion_read_back() {
    std::cout << "\na whole completion, written and read:\n";

    const std::string body = oa::completion("id-1", "tiny", 1789, "Paris.",
                                            oa::finish::length, 11, 3);

    const oa::answer a = oa::answer::parse(body);

    ok("the text", a.content == "Paris.", a.content);
    ok("  the id and model", a.id == "id-1" && a.model == "tiny");
    ok("  why it stopped", a.why == oa::finish::length);
    ok("  and both token counts",
       a.prompt_tokens == 11 && a.completion_tokens == 3,
       std::to_string(a.prompt_tokens) + "/" + std::to_string(a.completion_tokens));

    // Several servers omit usage; a reply without it is still a reply.
    const oa::answer none = oa::answer::parse(
        R"({"id":"x","model":"m","choices":[{"index":0,)"
        R"("message":{"role":"assistant","content":"hi"},"finish_reason":"stop"}]})");

    ok("a reply with no usage block is still read",
       none.content == "hi" && none.prompt_tokens == 0);

    bool threw = false;

    try { oa::answer::parse(R"({"id":"x"})"); }
    catch(json::exception&) { threw = true; }

    ok("but one with no choices is refused", threw);
}

static void chunks_read_back() {
    std::cout << "\nchunks, written and read:\n";

    {
        oa::delta d;

        d.role = true;

        const oa::delta back = oa::delta::parse(oa::chunk("i", "m", 1, d));

        ok("the first chunk says role and nothing else",
           back.role && back.content.empty() && !back.done);
    }

    {
        oa::delta d;

        d.content = "Par";

        const oa::delta back = oa::delta::parse(oa::chunk("i", "m", 1, d));

        ok("a content chunk carries its text",
           back.content == "Par" && !back.done && !back.role, back.content);
    }

    {
        oa::delta d;

        d.done = true;
        d.why = oa::finish::length;

        const oa::delta back = oa::delta::parse(oa::chunk("i", "m", 1, d));

        ok("the last chunk is finished, and says why",
           back.done && back.why == oa::finish::length);
    }

    // An absent finish_reason is null is "not finished", and that is the one
    // a hand-written parser gets wrong -- json-c cannot tell absent from null
    // and neither reading changes what it means.
    const oa::delta mid = oa::delta::parse(
        R"({"choices":[{"index":0,"delta":{"content":"x"}}]})");

    ok("an absent finish_reason means not finished", !mid.done && mid.content == "x");
}

static void a_stream_arriving_in_pieces() {
    std::cout << "\nevents, unframed as they arrive:\n";

    oa::event_reader r;

    // The whole reason this holds state: a read returns whatever bytes have
    // arrived, and the split falls wherever the network put it.
    const std::string whole =
        oa::event(R"({"a":1})") + oa::event(R"({"b":2})") + oa::done();

    std::vector<std::string> got;

    for(std::size_t i = 0; i < whole.size(); i++) {
        const std::vector<std::string> some = r.feed(whole.substr(i, 1));

        for(const std::string& e : some) got.push_back(e);
    }

    ok("a stream fed one byte at a time still yields whole events",
       got.size() == 2 && got[0] == R"({"a":1})" && got[1] == R"({"b":2})",
       std::to_string(got.size()) + " events");

    ok("  and [DONE] is reported rather than returned", r.done());

    ok("  leaving nothing held", r.pending() == 0,
       std::to_string(r.pending()));
}

static void what_a_stream_may_contain() {
    std::cout << "\nwhat else a stream may carry:\n";

    {
        // A heartbeat during a long generation is a comment line, and reading
        // one as data would be reading a keep-alive as a token.
        oa::event_reader r;

        const std::vector<std::string> got = r.feed(": ping\n\ndata: {\"a\":1}\n\n");

        ok("a comment is not an event", got.size() == 1 && got[0] == R"({"a":1})",
           std::to_string(got.size()) + " events");
    }

    {
        // CRLF, which a proxy may produce where jserve writes LF.
        oa::event_reader r;

        const std::vector<std::string> got = r.feed("data: {\"a\":1}\r\n\r\n");

        ok("CRLF frames an event as LF does", got.size() == 1 && got[0] == R"({"a":1})",
           got.empty() ? "none" : got[0]);
    }

    {
        // The SSE grammar joins several data lines with a newline.  Nothing
        // in this protocol sends one, which is why a client that assumed a
        // single line would look right until it met something else.
        oa::event_reader r;

        const std::vector<std::string> got = r.feed("data: one\ndata: two\n\n");

        ok("two data lines are one payload, joined by a newline",
           got.size() == 1 && got[0] == "one\ntwo",
           got.empty() ? "none" : got[0]);
    }

    {
        // event:, id: and retry: carry the framing's own meaning, not the
        // protocol's.
        oa::event_reader r;

        const std::vector<std::string> got =
            r.feed("event: message\nid: 7\nretry: 100\ndata: {\"a\":1}\n\n");

        ok("the other SSE fields are skipped",
           got.size() == 1 && got[0] == R"({"a":1})",
           got.empty() ? "none" : got[0]);
    }
}

static void a_refusal_read_back() {
    std::cout << "\na refusal, written and read:\n";

    oa::failure f;

    ok("an error body is recognised",
       oa::failure::parse(oa::error("no model called \"x\"",
                                    "invalid_request_error"), f));

    ok("  with the message the server wrote",
       f.message == "no model called \"x\"", f.message);

    ok("  and its type", f.type == "invalid_request_error", f.type);

    oa::failure other;

    ok("a completion is not an error",
       !oa::failure::parse(oa::completion("i", "m", 1, "hi",
                                          oa::finish::stop, 1, 1), other));

    // A proxy's HTML, or an empty response: not an error object, and saying
    // so is more use than an exception about JSON.
    ok("and neither is something that is not JSON at all",
       !oa::failure::parse("<html>502 Bad Gateway</html>", other));
}

int main() {
    std::cout << std::unitbuf;

    it_reads_a_request();
    absent_is_not_zero();
    content_may_be_parts();
    a_tool_request_is_visible();
    calls_are_read_out_of_the_text();
    what_it_refuses();
    it_writes_a_completion();
    it_writes_chunks();
    a_partial_character_is_refused();
    it_frames_events();
    it_lists_models();
    ids_are_unique();
    errors_are_unwrappable();
    a_request_survives_the_round_trip();
    absent_stays_absent();
    a_completion_read_back();
    chunks_read_back();
    a_stream_arriving_in_pieces();
    what_a_stream_may_contain();
    a_refusal_read_back();

    // What a green run does not establish.
    //
    // That any real client accepts these.  Every assertion here reads back
    // what this code wrote, with this code's idea of the format on both
    // sides -- which a consistently wrong implementation would also pass.
    // The format belongs to somebody else, and the only check that would
    // settle it is pointing aider or the openai client at a running server.
    //
    // Nothing about tool calling, which is not implemented: a request asking
    // for it is *recognised*, and answering one is a caller's problem.
    //
    // And nothing about HTTP.  These are strings; whether they reach a client
    // correctly framed is net::http::server's business and is tested there.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
