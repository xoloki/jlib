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
#include <algorithm>
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

    // Kept as text: a tool's parameters are a JSON Schema and there is no
    // struct that holds one without losing something.  See request::tools.
    if(root->has("tools") && root->kind("tools") == util::json::object::type_array)
        r.tools = root->arr("tools")->str();
    else if(root->has("functions") &&
            root->kind("functions") == util::json::object::type_array) {
        // The older spelling is a bare function list where `tools` wraps each
        // one in {"type":"function","function":{...}}.  Wrapped here so a
        // template sees one shape and the caller does not have to care which
        // spelling arrived.
        //
        // **Assembled as text rather than as objects.** `array::obj(i)`
        // borrows -- it hands back a handle with no reference of its own --
        // and `object::add` steals one, so moving a function from the parsed
        // request into a new array would leave both owning it and both
        // freeing it. The destination is a string either way.
        util::json::array::ptr in = root->arr("functions");

        std::string out = "[";

        for(int i = 0; i < in->size(); i++) {
            if(in->kind(unsigned(i)) != util::json::object::type_object) continue;

            if(out.size() > 1) out += ",";

            out += "{\"type\":\"function\",\"function\":";
            out += in->obj(unsigned(i))->str();
            out += "}";
        }

        r.tools = out + "]";
    }

    if(root->has("tool_choice")) {
        // A string or an object, both of which the protocol allows -- the
        // same shape hazard `stop` has above, so the object is asked about
        // first for the same reason.
        if(root->kind("tool_choice") == util::json::object::type_object)
            r.tool_choice = root->obj("tool_choice")->str();
        else
            r.tool_choice = root->get("tool_choice").str_or(std::string());
    }

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

std::vector<call> calls_in(const std::string& text, std::string& left)
{
    static const std::string OPEN = "<tool_call>";
    static const std::string CLOSE = "</tool_call>";

    std::vector<call> out;

    left.clear();

    std::size_t at = 0;

    for(;;) {
        const std::size_t b = text.find(OPEN, at);

        if(b == std::string::npos) { left += text.substr(at); break; }

        left += text.substr(at, b - at);

        const std::size_t body = b + OPEN.size();
        const std::size_t e = text.find(CLOSE, body);

        // An unterminated block is not a call.  The text is kept as content
        // rather than dropped: a reply cut short mid-call is what a client
        // needs to see to know it was cut short.
        if(e == std::string::npos) { left += text.substr(b); break; }

        const std::string json = text.substr(body, e - body);

        call c;

        try {
            util::json::object::ptr o = util::json::object::create(json);

            c.name = o->get("name").str_or(std::string());

            // `arguments` is an object here, where the wire format nests it
            // as a string.  Serialised back so both sides carry the same
            // thing -- see openai::call.
            if(o->has("arguments")) {
                if(o->kind("arguments") == util::json::object::type_object)
                    c.arguments = o->obj("arguments")->str();
                else
                    c.arguments = o->get("arguments").str_or(std::string());
            }
        }
        catch(std::exception&) {
            // Not JSON between the markers.  Not a call, and the text is kept
            // so the caller can see what the model actually said.
            left += text.substr(b, e + CLOSE.size() - b);

            at = e + CLOSE.size();

            continue;
        }

        // A block with no name is not a call, and the text is kept for the
        // same reason a malformed one is: dropping it would remove what the
        // model said without saying so, and a reader would see a reply that
        // was simply missing a sentence.
        if(c.name.empty()) left += text.substr(b, e + CLOSE.size() - b);
        else out.push_back(c);

        at = e + CLOSE.size();
    }

    return out;
}

namespace {

/**
 * How much of `s`'s tail could still be the start of `marker`.
 *
 * The longest proper prefix of `marker` that `s` ends with -- which is exactly
 * what must be held back, because the next token may complete it.
 */
std::size_t ambiguous_tail(const std::string& s, const std::string& marker)
{
    const std::size_t most = std::min(s.size(), marker.size() - 1);

    for(std::size_t k = most; k > 0; k--)
        if(s.compare(s.size() - k, k, marker, 0, k) == 0) return k;

    return 0;
}

}

std::string call_stream::feed(const std::string& text)
{
    static const std::string OPEN = "<tool_call>";
    static const std::string CLOSE = "</tool_call>";

    m_pending += text;

    std::string out;

    for(;;) {
        const std::size_t b = m_pending.find(OPEN);

        if(b == std::string::npos) {
            // No marker yet.  Everything but a tail that could become one is
            // safe to send.
            const std::size_t hold = ambiguous_tail(m_pending, OPEN);

            out += m_pending.substr(0, m_pending.size() - hold);
            m_pending = m_pending.substr(m_pending.size() - hold);

            break;
        }

        out += m_pending.substr(0, b);

        const std::size_t e = m_pending.find(CLOSE, b + OPEN.size());

        // An open marker with no close yet: hold it and wait.  This is the
        // one case that can hold more than a marker's worth, and it is
        // bounded by the call the model is writing.
        if(e == std::string::npos) { m_pending = m_pending.substr(b); break; }

        const std::string block = m_pending.substr(b, e + CLOSE.size() - b);

        // Parsed by the same function the non-streaming path uses, so the two
        // cannot disagree about what a call is.
        std::string leftover;

        const std::vector<call> got = calls_in(block, leftover);

        if(got.empty()) out += leftover;    // unreadable: it is content
        else {
            m_calls.insert(m_calls.end(), got.begin(), got.end());

            m_saw = true;
        }

        m_pending = m_pending.substr(e + CLOSE.size());
    }

    return out;
}

std::string call_stream::flush()
{
    const std::string out = m_pending;

    m_pending.clear();

    return out;
}

std::vector<call> call_stream::take()
{
    std::vector<call> out;

    out.swap(m_calls);

    return out;
}

const char* spell(finish f) {
    if(f == finish::length) return "length";
    if(f == finish::tool_calls) return "tool_calls";

    return "stop";
}

namespace {

/**
 * The `tool_calls` on a message, if any.
 *
 * Shaped `[{"id":..,"type":"function","function":{"name":..,"arguments":..}}]`,
 * where `arguments` is a **string** holding JSON rather than an object -- the
 * protocol nests it that way and a reader that expects an object gets nothing.
 * It is carried as that string; see openai::call.
 */
std::vector<call> calls_of(util::json::object::ptr m)
{
    std::vector<call> out;

    if(!m->has("tool_calls") ||
       m->kind("tool_calls") != util::json::object::type_array)
        return out;

    util::json::array::ptr a = m->arr("tool_calls");

    for(int i = 0; i < a->size(); i++) {
        if(a->kind(unsigned(i)) != util::json::object::type_object) continue;

        util::json::object::ptr one = a->obj(unsigned(i));

        call c;

        c.id = one->get("id").str_or(std::string());

        if(one->has("function") &&
           one->kind("function") == util::json::object::type_object) {
            util::json::object::ptr f = one->obj("function");

            c.name = f->get("name").str_or(std::string());
            c.arguments = f->get("arguments").str_or(std::string());
        }

        // A call with no name is not a call.  Dropped rather than carried as
        // an empty one, which a caller would have to check for anyway.
        if(!c.name.empty()) out.push_back(c);
    }

    return out;
}

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

namespace {

/** The `tool_calls` array, as the protocol shapes it. */
util::json::array::ptr calls_json(const std::vector<call>& calls)
{
    util::json::array::ptr a = util::json::array::create();

    for(std::size_t i = 0; i < calls.size(); i++) {
        util::json::object::ptr f = util::json::object::create();

        f->add("name", calls[i].name);

        // A **string** holding JSON, which is how the protocol nests it --
        // an object here is what a client that expects a string reads as
        // nothing.  See openai::call.
        f->add("arguments", calls[i].arguments.empty() ? std::string("{}")
                                                       : calls[i].arguments);

        util::json::object::ptr one = util::json::object::create();

        // An id is required by the protocol and is what a tool result is
        // matched back to, so one is invented when the model gave none --
        // which it always does, since the model emits a name and arguments
        // and nothing else.
        one->add("id", calls[i].id.empty()
                           ? "call_" + std::to_string(i) : calls[i].id);
        one->add("type", std::string("function"));
        one->add("function", f);

        a->add(one);
    }

    return a;
}

}

std::string completion(const std::string& id, const std::string& model,
                       std::int64_t created, const std::string& content,
                       finish why, unsigned int prompt_tokens,
                       unsigned int completion_tokens,
                       const std::vector<call>& calls)
{
    refuse_a_partial_character(content, "completion");

    util::json::object::ptr root = envelope(id, model, created,
                                            "chat.completion");

    util::json::object::ptr m = util::json::object::create();

    m->add("role", std::string("assistant"));
    m->add("content", content);

    if(!calls.empty()) m->add("tool_calls", calls_json(calls));

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

    // A whole call in one chunk rather than dribbled across several: by the
    // time it has been read out of the model's text it is complete, and the
    // streaming shape exists to avoid making a client wait rather than to
    // fragment for its own sake.  See delta::calls.
    if(!d.calls.empty()) dj->add("tool_calls", calls_json(d.calls));

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

    a.why = why == "length"      ? finish::length
          : why == "tool_calls"  ? finish::tool_calls
                                 : finish::stop;

    util::json::object::ptr m;

    try { m = choice->obj("message"); }
    catch(util::json::exception&) {}

    // A choice may carry content, calls, or both: the protocol allows a
    // sentence before a call and some models emit one.  Content stays empty
    // rather than throwing when there is none.
    if(m) {
        a.content = content_of(m);

        a.calls = calls_of(m);
    }

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
