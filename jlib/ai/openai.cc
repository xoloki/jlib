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
        // A string or an array of them, both of which the protocol allows.
        const std::string one = root->get("stop").str_or("");

        if(!one.empty()) r.stop.push_back(one);
        else {
            try {
                util::json::array::ptr s = root->arr("stop");

                for(int i = 0; i < s->size(); i++)
                    r.stop.push_back(std::string(s->get(unsigned(i))));
            }
            catch(util::json::exception&) {}
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

std::string completion(const std::string& id, const std::string& model,
                       std::int64_t created, const std::string& content,
                       finish why, unsigned int prompt_tokens,
                       unsigned int completion_tokens)
{
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
