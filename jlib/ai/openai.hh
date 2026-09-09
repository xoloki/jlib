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
 * is prose.  `tools`/`tool_calls` are a larger contract and are not here.
 *
 * ## What is deliberately not modelled
 *
 * `tools`, `tool_choice`, `functions`, `logprobs`, `n` above one, images, and
 * the `response_format` schemas.  A request carrying them is not refused --
 * they are ignored -- because a harness that sends `"n": 1` alongside things
 * this does not read should still work, and because refusing an unknown field
 * is how a server becomes brittle against a client that gained one.
 *
 * The cost of ignoring rather than refusing: a caller asking for `"n": 3` is
 * silently given one choice.  That is the wrong trade for `tools`, where
 * silence would look like a model that cannot call them -- so a caller that
 * cares should ask `request::wants_tools()` and say so itself.
 */
namespace openai {

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
     * Whether the caller asked for tools, which this does not implement.
     *
     * Exposed so a caller can refuse loudly.  Answering a tool request with
     * prose looks like a model too weak to call tools, which is a much harder
     * thing to diagnose than a 400 saying so.
     */
    bool wants_tools = false;

    /**
     * @throws util::json::exception if the body is not an object, or
     *         `messages` is missing or not an array of objects
     *
     * Lenient about everything else.  A message's `content` may be a string
     * or an array of parts, because the newer clients send the latter; the
     * text parts are joined and anything else in it is dropped.
     */
    static request parse(const std::string& body);
};

/** What ends a reply, as the protocol spells it. */
enum class finish { stop, length };

const char* spell(finish f);

/** One `chat.completion.chunk`'s delta. */
struct delta {
    /** The first chunk of a reply carries the role and no content. */
    bool role = false;

    std::string content;

    /** Set on the last chunk, which carries no content. */
    bool done = false;
    finish why = finish::stop;
};

/**
 * A whole `chat.completion`, for a request that did not ask to stream.
 *
 * @param created seconds since the epoch, taken by the caller so a test can
 *        be deterministic
 */
std::string completion(const std::string& id, const std::string& model,
                       std::int64_t created, const std::string& content,
                       finish why, unsigned int prompt_tokens,
                       unsigned int completion_tokens);

/** One `chat.completion.chunk`, for a request that did. */
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
