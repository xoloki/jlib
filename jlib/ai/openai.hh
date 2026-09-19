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

#ifndef JLIB_AI_OPENAI_HH
#define JLIB_AI_OPENAI_HH

#include <jlib/ai/chat.hh>

#include <cstdint>
#include <string>
#include <vector>

namespace jlib {
namespace ai {

/**
 * OpenAI's chat-completions wire format, as JSON and nothing else.
 *
 * No HTTP here and no model: these turn a request body into a struct and a
 * reply into the bytes that go on the wire.  That is deliberate -- the format
 * is somebody else's and every field is a chance to be subtly wrong, so it is
 * worth testing without a socket or a GGUF in the way.
 *
 * ## What it is for
 *
 * `aider` and the other coding harnesses speak this.  aider in particular is
 * the target because its **`whole` edit format needs no tool calling**: the
 * model answers with a filename and a fenced block of the whole file, which
 * is prose.  That is still the shortest path to a working harness, and it is
 * the one #242 took.
 *
 * `tools` and `tool_calls` are here as of #311, because the models already
 * speak them: Qwen 2.5's own chat template renders a tool list and asks for
 * `<tool_call>` blocks back, and jlib was discarding a protocol both ends
 * already knew.
 *
 * ## What is deliberately not modelled
 *
 * `logprobs`, `n` above one, images, and the `response_format` schemas.  A
 * request carrying them is not refused -- they are ignored -- because a
 * harness that sends `"n": 1` alongside things this does not read should still
 * work, and because refusing an unknown field is how a server becomes brittle
 * against a client that gained one.
 *
 * The cost of ignoring rather than refusing: a caller asking for `"n": 3` is
 * silently given one choice.  **`tool_choice` is carried but not obeyed** for
 * the same reason it was the exception before: silence there looks like a
 * model that would not call, so a server should read it and refuse what it
 * cannot do rather than answer as if it had complied.
 */
namespace openai {

/**
 * One call, as the protocol spells it on the wire.
 *
 * `arguments` is the raw JSON text of the object rather than a parsed
 * structure, for the same reason `request::tools` is: its shape belongs to
 * whoever wrote the tool. A client that knows the tool parses it; this does
 * not, and cannot.
 *
 * **Streaming delivers this in pieces.** A chunk carries an `index`, and the
 * name arrives once while `arguments` accumulates across chunks -- so a
 * reader has to append rather than assign. See delta.
 */
struct call {
    std::string id;
    std::string name;
    std::string arguments;
};

/**
 * The calls in a model's own output, by the `<tool_call>` convention.
 *
 * A model does not emit JSON tool calls: it emits **text**, in whatever markup
 * its template asked for. Qwen 2.5's asks for
 *
 *     <tool_call>
 *     {"name": <function-name>, "arguments": <args-json-object>}
 *     </tool_call>
 *
 * and this reads that. It is the convention ChatML-derived templates share,
 * and it is the one jlib's own models use.
 *
 * **It is not universal, and nothing here can make it so.** Llama 3.1 emits a
 * bare JSON object after `<|python_tag|>`; Mistral uses `[TOOL_CALLS]`. Those
 * are per-architecture constants the file does not state -- the same shape of
 * problem as the RoPE layout, which #180 is the worked example of getting
 * wrong. A model whose template asks for something else will produce text this
 * does not recognise, and the calls come back empty rather than mangled.
 *
 * @param text what the model generated
 * @param[out] left the text with the call blocks removed, which is what a
 *        client should be shown as content
 */
std::vector<call> calls_in(const std::string& text, std::string& left);

/** A parsed POST /v1/chat/completions body. */
struct request {
    std::string model;
    std::vector<message> messages;

    /** Server-sent events rather than one JSON object. */
    bool stream = false;

    /**
     * Absent means "whatever the server would have chosen", which is not the
     * same as zero -- zero is greedy and is a thing a caller can ask for.
     */
    bool has_temperature = false;
    float temperature = 0;

    /** 0 is absent; the server decides what a reply may cost. */
    unsigned int max_tokens = 0;

    /** Extra strings that end a reply, beyond the model's own end token. */
    std::vector<std::string> stop;

    /**
     * Whether the caller asked for tools.
     *
     * True for either spelling -- `tools`, and the older `functions` that
     * some clients still send.
     */
    bool wants_tools = false;

    /**
     * The tool definitions, as the caller wrote them: a JSON array, or empty.
     *
     * **Not modelled, on purpose.** A tool's `parameters` is a JSON Schema
     * belonging to whoever wrote the tool, so there is no struct that can
     * hold one without losing something. The text goes to `chat::format`,
     * which renders it through the model's own template.
     *
     * The older `functions` spelling is lifted into the same field, wrapped
     * so it has the shape `tools` has -- a client sending the old name gets
     * the same prompt as one sending the new.
     */
    std::string tools;

    /**
     * `tool_choice`, if the caller sent one: "auto", "none", "required", or a
     * JSON object naming a function.
     *
     * Carried rather than obeyed. A server that ignores it answers a
     * "required" with prose and looks like a model that would not call --
     * which is the failure this whole field exists to let a caller refuse
     * loudly instead of diagnose.
     */
    std::string tool_choice;

    /**
     * @throws util::json::exception if the body is not an object, or
     *         `messages` is missing or not an array of objects
     *
     * Lenient about everything else.  A message's `content` may be a string
     * or an array of parts, because the newer clients send the latter; the
     * text parts are joined and anything else in it is dropped.
     */
    static request parse(const std::string& body);

    /**
     * The body to POST, which is the mirror of parse().
     *
     * Absent rather than defaulted wherever the protocol distinguishes the
     * two: no `temperature` unless one was set, no `max_tokens` at zero, no
     * `stop` when the list is empty.  A server reads an absent field as "you
     * decide" and a present one as an instruction, and sending 0.0 because
     * that is what the struct was initialised to is how a client silently
     * asks for greedy sampling.
     *
     * `stream` is written only when true, for the same reason -- and it is
     * why util::json grew a boolean: json-c stores 1 for an int and no
     * OpenAI-compatible server reads 1 as true.
     *
     * `wants_tools` is **not** written.  It is a flag saying a request asked
     * for tools, not a description of which ones, and this namespace models
     * neither.
     */
    std::string str() const;
};

/** What ends a reply, as the protocol spells it. */
/**
 * Why a reply ended.
 *
 * `tool_calls` is the protocol's own spelling and means the model stopped
 * because it asked for a call rather than because it ran out of things to say
 * -- a client that treats it as `stop` will answer the user with an empty
 * turn.
 */
enum class finish { stop, length, tool_calls };

const char* spell(finish f);

/** One `chat.completion.chunk`'s delta. */
struct delta {
    /** The first chunk of a reply carries the role and no content. */
    bool role = false;

    std::string content;

    /** Set on the last chunk, which carries no content. */
    bool done = false;
    finish why = finish::stop;

    /**
     * Read one `chat.completion.chunk`.
     *
     * @throws util::json::exception if it is not an object with a choices
     *         array
     *
     * **An absent `finish_reason` means not finished**, which is how the
     * protocol spells null -- json-c stores a JSON null as no value at all,
     * so the two are indistinguishable from here and both mean the same
     * thing.  A chunk that carries one is the last.
     */
    static delta parse(const std::string& json);
};

/**
 * A whole `chat.completion`, for a request that did not ask to stream.
 *
 * @param created seconds since the epoch, taken by the caller so a test can
 *        be deterministic
 *
 * @throws util::json::exception if `content` stops in the middle of a UTF-8
 *         character.  See chunk() below, which is where that happens -- but a
 *         whole reply ends mid-character too when a generation stops between
 *         two byte-fallback tokens.
 */
std::string completion(const std::string& id, const std::string& model,
                       std::int64_t created, const std::string& content,
                       finish why, unsigned int prompt_tokens,
                       unsigned int completion_tokens);

/**
 * One `chat.completion.chunk`, for a request that did.
 *
 * @throws util::json::exception if the delta's content stops in the middle of
 *         a UTF-8 character.
 *
 * **A fragment of a stream still has to be a whole string.**  There is no
 * escape for half a character -- `\uXXXX` needs a codepoint and half of one
 * does not have it -- so the byte goes out raw, and a JSON text that is not
 * valid UTF-8 is not valid JSON.  The client decodes the event as text before
 * parsing it, so the failure surfaces as an exception in *its* code with
 * nothing pointing back here.
 *
 * A real thing that happened rather than a precaution: a byte-fallback
 * vocabulary returns one byte per token, jserve sent one event per token, and
 * three emoji made twelve unparseable events (#249).  Whoever decides chunk
 * boundaries has to hold a character until it is whole -- `util::utf8_stream`
 * is that -- and this refuses so the next caller finds out here rather than
 * from a stranger's traceback.
 */
std::string chunk(const std::string& id, const std::string& model,
                  std::int64_t created, const delta& d);

/**
 * One server-sent event, framed.
 *
 * `data: ` and the two newlines that end an event.  Here rather than in the
 * caller because forgetting the second newline produces a stream that looks
 * right and never delivers.
 */
std::string event(const std::string& json);

/** The event that ends a stream: `data: [DONE]`. */
std::string done();

/** A `GET /v1/models` body. */
std::string models(const std::vector<std::string>& names,
                   std::int64_t created);

/** An `id`, which the protocol wants unique per completion. */
std::string new_id();

/**
 * A whole `chat.completion`, read.
 *
 * The mirror of completion() above, and the reply to a request that did not
 * ask to stream.
 */
struct answer {
    std::string id;
    std::string model;
    std::string content;

    /**
     * What the model asked to call, if anything.
     *
     * A reply is one or the other in practice -- content, or calls -- but the
     * protocol permits both and some models emit a sentence before the call,
     * so this does not assume.
     */
    std::vector<call> calls;

    finish why = finish::stop;

    unsigned int prompt_tokens = 0;
    unsigned int completion_tokens = 0;

    /**
     * @throws util::json::exception if the body is not an object, or has no
     *         choices array
     *
     * Lenient about the rest: a reply with no usage block is a reply, and
     * several servers omit it.  `content` may be absent on a choice that was
     * a tool call, and comes back empty rather than throwing.
     */
    static answer parse(const std::string& body);
};

/**
 * What a refusal says, when the status was not 2xx.
 *
 * Separate from answer because it is a different shape, and a caller that
 * treats a 400 body as a completion gets an exception where the useful thing
 * is the message the server wrote.
 */
struct failure {
    std::string message;
    std::string type;

    /**
     * @return whether `body` is an error object, in which case f holds it
     *
     * Does not throw.  A body that is neither a completion nor an error --
     * a proxy's HTML, an empty response -- is not an error *object*, and
     * saying so is more use to a caller than an exception about JSON.
     */
    static bool parse(const std::string& body, failure& f);
};

/**
 * Server-sent events, unframed as they arrive.
 *
 * The mirror of event() above, and incremental because that is what a stream
 * is: a read returns whatever bytes have arrived, which may be half an event,
 * or three of them, and the split falls wherever the network put it.
 *
 * ## What it handles, and why each one is here
 *
 * - **A payload split across reads.** The whole reason this holds state.
 * - **Both line endings.** The SSE grammar allows CRLF, LF and CR; jserve
 *   writes LF and a server behind a proxy may not.
 * - **Comment lines.** A line beginning with `:` is a comment, which is what
 *   a heartbeat is -- and an endpoint that sends one during a long generation
 *   is doing the right thing, so treating it as data would be reading a
 *   keep-alive as a token.
 * - **More than one `data:` line in an event**, joined with a newline, as the
 *   SSE grammar says. Nothing in the OpenAI protocol sends multi-line data,
 *   which is exactly why a client that assumed one line would look correct
 *   until it met something else.
 * - **`event:`, `id:` and `retry:`**, which are skipped: the protocol carries
 *   its meaning in the payload.
 *
 * `[DONE]` is **not** SSE, it is OpenAI's sentinel, and it is reported
 * through done() rather than returned as a payload -- a caller that had to
 * string-compare each payload against it would be one forgotten check away
 * from parsing it as JSON.
 */
class event_reader {
public:
    /** The complete event payloads in what has arrived so far. */
    std::vector<std::string> feed(const std::string& bytes);

    /** Whether the stream said `[DONE]`.  It may arrive mid-feed. */
    bool done() const { return m_done; }

    /** Bytes held because an event is incomplete.  For a test to look at. */
    std::size_t pending() const { return m_held.size(); }

private:
    std::string m_held;
    bool m_done = false;
};

/**
 * An error, in the shape the OpenAI clients unwrap.
 *
 * They look for `error.message` and show it; a bare string leaves a caller
 * reading "unknown error" and guessing.
 */
std::string error(const std::string& message, const std::string& type);

}

}
}

#endif // JLIB_AI_OPENAI_HH
