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

#include <jlib/ai/openai.hh>

#include <jlib/util/utf8.hh>

#include <jlib/util/json.hh>

#include <atomic>
#include <sstream>

namespace jlib {
namespace ai {
namespace openai {

namespace {

/**
 * A message's content, which may be a string or an array of parts.
 *
 * The older clients send a string and the newer ones send
 * `[{"type":"text","text":"..."}]`.  Both are accepted and the text parts are
 * joined: refusing the second would be correct by an older reading of the
 * protocol and useless against a client that has moved on.
 */
std::string content_of(const util::json::object::ptr& m) {
    if(!m->has("content")) return std::string();

    // The array is asked for **first**, and that order is the whole of it:
    // json-c's string accessor is lenient and hands back a *serialisation*
    // of whatever it finds, so asking for a string first turns an array of
    // parts into the literal text `[ { "type": "text", ... } ]` and reports
    // success.  arr() checks the type and throws, so this branch is decided
    // by what the document says rather than by what a conversion tolerates.
    try {
        util::json::array::ptr parts = m->arr("content");

        std::string out;

        for(int i = 0; i < parts->size(); i++) {
            util::json::object::ptr part = parts->obj(unsigned(i));

            if(!part) continue;

            // Anything that is not text -- an image, a file -- is dropped
            // rather than refused; a model that cannot see it would have
            // ignored it anyway.
            if(part->has("text")) out += std::string(part->get("text"));
        }

        return out;
    }
    catch(util::json::exception&) {}

    return m->get("content").str_or(std::string());
}

}

request request::parse(const std::string& body) {
    request r;

    util::json::object::ptr root = util::json::object::create(body);

    if(!root)
        throw util::json::exception("a chat completion request that is not "
                                    "an object");

    r.model = root->get("model").str_or("");
    r.stream = root->get("stream").bool_or(false);

    if(root->has("temperature")) {
        r.has_temperature = true;
        r.temperature = float(root->get("temperature").double_or(0));
    }

    const std::int64_t cap = root->get("max_tokens").int_or(0);

    // Negative is nonsense rather than a request for nothing, and clamping is
    // friendlier than throwing over a field the caller may not have meant.
    if(cap > 0) r.max_tokens = unsigned(cap);

    r.wants_tools = root->has("tools") || root->has("functions");

    if(root->has("stop")) {
        // A string or an array of them, both of which the protocol allows --
        // and the array is asked about **first**.
        //
        // It used to be the other way round, and the array branch was dead
        // code: str_or() on an array returns the array's own serialised text,
        // which is not empty, so `["\n\n","END"]` became one stop string
        // spelled `[ "\n\n", "END" ]` and matched nothing.  A client's stop
        // sequences were silently ignored.  arr() throws when it is not an
        // array, so this order cannot make the mirror-image mistake.
        bool listed = false;

        try {
            util::json::array::ptr s = root->arr("stop");

            if(s) {
                for(int i = 0; i < s->size(); i++)
                    r.stop.push_back(std::string(s->get(unsigned(i))));

                listed = true;
            }
        }
        catch(util::json::exception&) {}

        if(!listed) {
            const std::string one = root->get("stop").str_or("");

            if(!one.empty()) r.stop.push_back(one);
        }
    }

    util::json::array::ptr messages;

    try { messages = root->arr("messages"); }
    catch(util::json::exception&) {}

    if(!messages)
        throw util::json::exception("a chat completion request with no "
                                    "messages array");

    for(int i = 0; i < messages->size(); i++) {
        util::json::object::ptr m = messages->obj(unsigned(i));

        if(!m) continue;

        message turn;

        turn.role = m->get("role").str_or("user");
        turn.content = content_of(m);

        r.messages.push_back(turn);
    }

    return r;
}

const char* spell(finish f) {
    return f == finish::length ? "length" : "stop";
}

namespace {

/** The fields every completion and chunk carries. */
util::json::object::ptr envelope(const std::string& id,
                                 const std::string& model,
                                 std::int64_t created, const char* object)
{
    util::json::object::ptr o = util::json::object::create();

    o->add("id", id);
    o->add("object", std::string(object));
    // As an integer, not a double.  The facade's double overload writes
    // "1788937890.0", which a strict client rejects and a lenient one
    // silently coerces -- and unsigned int holds an epoch second until 2106,
    // which is longer than this format will be current.
    o->add("created", (unsigned int)(created));
    o->add("model", model);

    return o;
}

}

namespace {

/**
 * Refuse content that stops in the middle of a character.
 *
 * There is no escape for half a character: `\uXXXX` needs a codepoint and
 * half of one does not have it, so what goes out is a raw byte inside a JSON
 * string -- and a JSON text that is not valid UTF-8 is not valid JSON (RFC
 * 8259 8.1).  Every client decodes the SSE line as text *before* parsing it,
 * so the failure lands as an exception in somebody else's program, a long way
 * from the endpoint that caused it.
 *
 * jserve streamed one event per token and a byte-fallback vocabulary gives one
 * byte per token, which made twelve broken events out of one three-emoji reply
 * (#249).  It holds whole characters now; this is here so that the next
 * caller to stream a fragment finds out at the point the format is defined
 * rather than in a client's traceback.
 *
 * json::exception because the statement is exactly that: this cannot be a
 * JSON string.
 */
void refuse_a_partial_character(const std::string& content, const char* what) {
    if(!jlib::util::utf8_ends_mid_character(content))
        return;

    throw util::json::exception(std::string("a chat ") + what +
                                " whose content ends in the middle of a UTF-8 "
                                "character, which no JSON string can hold");
}

}

std::string completion(const std::string& id, const std::string& model,
                       std::int64_t created, const std::string& content,
                       finish why, unsigned int prompt_tokens,
                       unsigned int completion_tokens)
{
    refuse_a_partial_character(content, "completion");

    util::json::object::ptr root = envelope(id, model, created,
                                            "chat.completion");

    util::json::object::ptr m = util::json::object::create();

    m->add("role", std::string("assistant"));
    m->add("content", content);

    util::json::object::ptr choice = util::json::object::create();

    choice->add("index", 0);
    choice->add("message", m);
    choice->add("finish_reason", std::string(spell(why)));

    util::json::array::ptr choices = util::json::array::create();

    choices->add(choice);

    root->add("choices", choices);

    util::json::object::ptr usage = util::json::object::create();

    usage->add("prompt_tokens", prompt_tokens);
    usage->add("completion_tokens", completion_tokens);
    usage->add("total_tokens", prompt_tokens + completion_tokens);

    root->add("usage", usage);

    return root->str();
}

std::string chunk(const std::string& id, const std::string& model,
                  std::int64_t created, const delta& d)
{
    refuse_a_partial_character(d.content, "completion chunk");

    util::json::object::ptr root = envelope(id, model, created,
                                            "chat.completion.chunk");

    util::json::object::ptr dj = util::json::object::create();

    if(d.role) dj->add("role", std::string("assistant"));

    // Present even when empty on a content chunk, absent on the last one --
    // which is what the clients expect and what stops a final empty string
    // being appended to a reply.
    if(!d.done) dj->add("content", d.content);

    util::json::object::ptr choice = util::json::object::create();

    choice->add("index", 0);
    choice->add("delta", dj);

    // Only on the last.  json-c stores a JSON null as no value at all, so
    // leaving it out is how null is spelled -- and the clients read an absent
    // finish_reason as "not finished", which is what it means.
    if(d.done) choice->add("finish_reason", std::string(spell(d.why)));

    util::json::array::ptr choices = util::json::array::create();

    choices->add(choice);

    root->add("choices", choices);

    return root->str();
}

std::string request::str() const {
    util::json::object::ptr root = util::json::object::create();

    root->add("model", model);

    util::json::array::ptr turns = util::json::array::create();

    for(const message& m : messages) {
        util::json::object::ptr t = util::json::object::create();

        t->add("role", m.role);
        t->add("content", m.content);

        turns->add(t);
    }

    root->add("messages", turns);

    // Every one of these is written only when it was asked for.  A server
    // reads an absent field as "you decide"; writing the struct's zero would
    // be asking for greedy sampling and a one-token reply.
    if(stream) root->add("stream", true);

    if(has_temperature) root->add("temperature", double(temperature));

    if(max_tokens) root->add("max_tokens", max_tokens);

    if(!stop.empty()) {
        util::json::array::ptr s = util::json::array::create();

        for(const std::string& one : stop) s->add(one);

        root->add("stop", s);
    }

    return root->str();
}

delta delta::parse(const std::string& json) {
    delta d;

    util::json::object::ptr root = util::json::object::create(json);

    if(!root)
        throw util::json::exception("a chat completion chunk that is not an "
                                    "object");

    util::json::array::ptr choices;

    try { choices = root->arr("choices"); }
    catch(util::json::exception&) {}

    if(!choices || !choices->size())
        throw util::json::exception("a chat completion chunk with no choices");

    util::json::object::ptr choice = choices->obj(0);

    // Absent is null is "not finished".  json-c stores a JSON null as no
    // value at all, so there is nothing here to tell the two apart -- and
    // nothing that needs to, since they mean the same thing.
    const std::string why = choice->get("finish_reason").str_or("");

    if(!why.empty()) {
        d.done = true;
        d.why = why == "length" ? finish::length : finish::stop;
    }

    util::json::object::ptr dj;

    try { dj = choice->obj("delta"); }
    catch(util::json::exception&) {}

    if(dj) {
        d.role = !dj->get("role").str_or("").empty();
        d.content = dj->get("content").str_or("");
    }

    return d;
}

answer answer::parse(const std::string& body) {
    answer a;

    util::json::object::ptr root = util::json::object::create(body);

    if(!root)
        throw util::json::exception("a chat completion that is not an object");

    a.id = root->get("id").str_or("");
    a.model = root->get("model").str_or("");

    util::json::array::ptr choices;

    try { choices = root->arr("choices"); }
    catch(util::json::exception&) {}

    if(!choices || !choices->size())
        throw util::json::exception("a chat completion with no choices");

    util::json::object::ptr choice = choices->obj(0);

    const std::string why = choice->get("finish_reason").str_or("");

    a.why = why == "length" ? finish::length : finish::stop;

    util::json::object::ptr m;

    try { m = choice->obj("message"); }
    catch(util::json::exception&) {}

    // A choice with no content is a tool call, which this namespace does not
    // model.  Empty rather than an exception: a caller that asked for no
    // tools will not be given one, and a caller that did was told this
    // server refuses them.
    if(m) a.content = content_of(m);

    util::json::object::ptr usage;

    try { usage = root->obj("usage"); }
    catch(util::json::exception&) {}

    // Several servers omit it, and a reply without a token count is still a
    // reply.
    if(usage) {
        a.prompt_tokens = unsigned(usage->get("prompt_tokens").int_or(0));
        a.completion_tokens = unsigned(usage->get("completion_tokens").int_or(0));
    }

    return a;
}

bool failure::parse(const std::string& body, failure& f) {
    util::json::object::ptr root;

    // A proxy's HTML or an empty response is not an error *object*, and
    // create() throws on it rather than returning null.  Saying "not an
    // error" is more use to a caller than an exception about JSON: it still
    // has a status code, and that is what it will report.
    try { root = util::json::object::create(body); }
    catch(util::json::exception&) { return false; }

    if(!root) return false;

    util::json::object::ptr e;

    try { e = root->obj("error"); }
    catch(util::json::exception&) { return false; }

    if(!e) return false;

    f.message = e->get("message").str_or("");
    f.type = e->get("type").str_or("");

    return true;
}

std::vector<std::string> event_reader::feed(const std::string& bytes) {
    std::vector<std::string> out;

    m_held += bytes;

    for(;;) {
        // An event ends at a blank line, in whichever spelling arrived.  CRLF
        // is checked first: "\r\n\r\n" contains "\n\n" at an offset, and
        // taking the shorter match would leave a stray CR at the head of the
        // next event.
        std::string::size_type at = m_held.find("\r\n\r\n");
        std::size_t skip = 4;

        const std::string::size_type lf = m_held.find("\n\n");

        if(lf != std::string::npos && (at == std::string::npos || lf < at)) {
            at = lf;
            skip = 2;
        }

        if(at == std::string::npos) break;

        const std::string one = m_held.substr(0, at);

        m_held.erase(0, at + skip);

        std::string data;
        bool any = false;

        std::string::size_type from = 0;

        while(from <= one.size()) {
            std::string::size_type end = one.find_first_of("\r\n", from);

            if(end == std::string::npos) end = one.size();

            std::string line = one.substr(from, end - from);

            from = end + 1;

            // A CR followed by an LF is one terminator, not two empty lines.
            if(from < one.size() && one[end] == '\r' && one[from] == '\n')
                from++;

            if(line.empty() || line[0] == ':') continue;   // blank, or a comment

            if(line.compare(0, 5, "data:") != 0) continue; // event:, id:, retry:

            std::string value = line.substr(5);

            // One optional space after the colon belongs to the framing.
            if(!value.empty() && value[0] == ' ') value.erase(0, 1);

            data += any ? "\n" + value : value;
            any = true;
        }

        if(!any) continue;

        // OpenAI's sentinel rather than SSE's: reported, never returned, so a
        // caller cannot feed it to a JSON parser by forgetting to check.
        if(data == "[DONE]") {
            m_done = true;

            continue;
        }

        out.push_back(data);
    }

    return out;
}

std::string event(const std::string& json) {
    // Two newlines.  One ends the line and the second ends the event, and a
    // stream missing the second parses as one event that never completes.
    return "data: " + json + "\n\n";
}

std::string done() { return "data: [DONE]\n\n"; }

std::string models(const std::vector<std::string>& names,
                   std::int64_t created)
{
    util::json::object::ptr root = util::json::object::create();

    root->add("object", std::string("list"));

    util::json::array::ptr data = util::json::array::create();

    for(const std::string& n : names) {
        util::json::object::ptr m = util::json::object::create();

        m->add("id", n);
        m->add("object", std::string("model"));
        m->add("created", (unsigned int)(created));
        m->add("owned_by", std::string("jlib"));

        data->add(m);
    }

    root->add("data", data);

    return root->str();
}

std::string new_id() {
    // Unique within a process, which is what the field is for -- a client
    // matches chunks to the request it made.  Not a UUID: nothing here is
    // asked to be unique across processes or to be unguessable.
    static std::atomic<unsigned long> next(0);

    std::ostringstream o;

    o << "chatcmpl-jlib" << ++next;

    return o.str();
}

std::string error(const std::string& message, const std::string& type) {
    util::json::object::ptr e = util::json::object::create();

    e->add("message", message);
    e->add("type", type);

    util::json::object::ptr root = util::json::object::create();

    root->add("error", e);

    return root->str();
}

}
}
}
