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


#include <jlib/ai/chat.hh>

#include <cctype>
#include <sstream>

namespace jlib {
namespace ai {

namespace {

/** Whitespace-trimmed, for the did-this-turn-survive check. */
std::string squeeze(const std::string& s)
{
    std::size_t b = 0, e = s.size();

    while(b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while(e > b && std::isspace(static_cast<unsigned char>(s[e-1]))) e--;

    return s.substr(b, e - b);
}

/**
 * The context a chat template expects to be rendered against.
 *
 * The names are the ones the ecosystem settled on -- `messages`, `eos_token`,
 * `add_generation_prompt` -- because templates are written against
 * transformers and use them without asking.
 *
 * The provenance flags are the load-bearing part.  A role name and the
 * end-of-sequence marker are the template's own text; a message's content is
 * a stranger's, and is the one thing here marked as not the template's.
 */
util::jinja::value context(const std::vector<message>& turns,
                           const std::string& eos,
                           bool add_generation_prompt)
{
    std::vector<util::jinja::value> msgs;

    for(std::size_t i = 0; i < turns.size(); i++) {
        std::map<std::string, util::jinja::value> m;

        m["role"] = util::jinja::value(turns[i].role, true);
        m["content"] = util::jinja::value(turns[i].content, false);

        msgs.push_back(util::jinja::value::of(m));
    }

    std::map<std::string, util::jinja::value> c;

    c["messages"] = util::jinja::value::of(msgs);
    c["eos_token"] = util::jinja::value(eos, true);

    // Empty, deliberately.  encode() puts the beginning-of-sequence token in
    // by id, so a template that also writes bos_token would give the model
    // two of them.  A template that writes it gets nothing here and the id
    // still arrives, which is the behaviour the scanner had.
    c["bos_token"] = util::jinja::value(std::string(), true);

    c["add_generation_prompt"] = util::jinja::value(add_generation_prompt);

    return util::jinja::value::of(c);
}

/**
 * Did every turn reach the output?
 *
 * See the note in chat.hh.  A renderer drops a turn whose role the template
 * does not name, in silence, and this is what the scanner's "no marker for
 * that role" check becomes.
 */
void check_nothing_vanished(const std::vector<message>& turns,
                            const util::jinja::text& out)
{
    // Walked in order, consuming spans, rather than searched for in one
    // concatenated blob.  Concatenating loses two things: that a turn's text
    // must appear *after* the previous turn's, and that each turn needs a span
    // of its own.  Without them, "Hi" is satisfied by the "Hi" inside a later
    // turn's "Hi there", and a template rendering only the last message passes
    // as long as the text happened to occur somewhere.
    std::size_t at = 0;

    for(std::size_t i = 0; i < turns.size(); i++) {
        const std::string want = squeeze(turns[i].content);

        // An empty message contributes no span at all -- there is nothing for
        // it to have left behind, so nothing to look for.  A template that
        // drops an empty turn is therefore invisible here, which is a real
        // gap and not a large one: an empty turn carries nothing the model
        // needed.
        if(want.empty()) continue;

        bool found = false;

        while(at < out.size() && !found) {
            if(!out[at].literal &&
               out[at].text.find(want) != std::string::npos)
                found = true;

            at++;
        }

        if(found) continue;

        std::ostringstream e;

        e << "the template rendered nothing for the message with role '"
          << turns[i].role << "' -- it names no branch for that role, so the "
          << "turn would have been dropped in silence";

        throw chat::exception(e.str());
    }
}

}

chat::chat(const gguf& g, const std::string& eos)
    : chat(g.has("tokenizer.chat_template")
               ? g.str("tokenizer.chat_template")
               : throw exception("the file carries no chat template, so there "
                                 "is no way to know how it wants a "
                                 "conversation laid out"),
           eos)
{}

chat::chat(const std::string& tmpl, const std::string& eos)
    : m_eos(eos),
      m_tmpl([&tmpl] {
          // Rethrown as a chat::exception so that a caller holding a model
          // file does not have to know which parser refused it.
          try { return util::jinja::tmpl(tmpl); }
          catch(std::exception& e) { throw exception(e.what()); }
      }())
{}

std::string chat::format(const std::vector<message>& turns,
                         bool add_generation_prompt) const
{
    const util::jinja::text out =
        m_tmpl.render(context(turns, m_eos, add_generation_prompt));

    check_nothing_vanished(turns, out);

    return util::jinja::flatten(out);
}

std::vector<int> chat::encode(const std::vector<message>& turns,
                              const tokenizer& tok,
                              bool add_generation_prompt) const
{
    const util::jinja::text out =
        m_tmpl.render(context(turns, m_eos, add_generation_prompt));

    check_nothing_vanished(turns, out);

    std::vector<int> ids;

    // The file decides, not the vocabulary: Qwen names a bos token and asks
    // for it not to be used.  See tokenizer::adds_bos.
    if(tok.bos() >= 0 && tok.adds_bos()) ids.push_back(tok.bos());

    // The dummy prefix belongs to the start of the whole prompt, so it goes to
    // whichever span turns out to be first.
    bool first = true;

    for(std::size_t i = 0; i < out.size(); i++) {
        // Here is the whole reason render() returns spans.  The template's own
        // text may spell a control token and mean it -- "</s>" written by the
        // template is the end-of-sequence token.  A message's content may
        // spell the same four characters and must not: a user who types
        // "</s>" has typed four characters.
        tok.append(out[i].text, first, out[i].literal, ids);

        first = false;
    }

    return ids;
}

}
}
