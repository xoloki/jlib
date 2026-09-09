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

/** A request this cannot honour has to be visible, not silently answered. */
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

int main() {
    std::cout << std::unitbuf;

    it_reads_a_request();
    absent_is_not_zero();
    content_may_be_parts();
    a_tool_request_is_visible();
    what_it_refuses();
    it_writes_a_completion();
    it_writes_chunks();
    it_frames_events();
    it_lists_models();
    ids_are_unique();
    errors_are_unwrappable();

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
