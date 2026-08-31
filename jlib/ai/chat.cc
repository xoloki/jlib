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

#include <sstream>

namespace jlib {
namespace ai {

chat::chat(const gguf& g, const std::string& eos)
    : chat(g.has("tokenizer.chat_template")
               ? g.str("tokenizer.chat_template")
               : throw exception("the file carries no chat template, so there "
                                 "is no way to know how it wants a "
                                 "conversation laid out"),
           eos)
{
}

chat::chat(const std::string& tmpl, const std::string& eos)
    : m_template(tmpl),
      m_eos(eos)
{

    // Every <|...|> in the template, in order, with the inner text taken as
    // the role.  That holds for the Zephyr family, where the marker is named
    // after the turn it opens; where it does not hold, the checks below fail
    // and the constructor throws rather than laying turns out wrongly.
    for(std::size_t at = 0; ; ) {
        const std::size_t open = m_template.find("<|", at);

        if(open == std::string::npos) break;

        const std::size_t close = m_template.find("|>", open + 2);

        if(close == std::string::npos) break;

        const std::string role = m_template.substr(open + 2, close - open - 2);

        at = close + 2;

        // Every marker is a candidate role, with no filtering on what it looks
        // like.  There was a check here rejecting anything but lowercase and
        // underscores, on the theory that it kept ChatML's "<|im_start|>" from
        // registering a role -- and a mutation showed it rejected nothing that
        // mattered.  ChatML is caught below by not naming user and assistant,
        // and "im_start" would have passed the check anyway.
        //
        // A stray marker such as "<|endoftext|>" therefore does appear in
        // roles(), and is harmless: format() only ever looks up the roles a
        // caller names, and a caller naming "endoftext" has said what it meant.
        if(role.empty()) continue;

        if(m_marker.find(role) == m_marker.end()) {
            m_marker[role] = "<|" + role + "|>";
            m_roles.push_back(role);
        }
    }

    // The two a conversation cannot do without.  A template that names neither
    // is not this shape at all, and one naming only a system prompt is some
    // other arrangement entirely.
    if(m_marker.find("user") == m_marker.end() ||
       m_marker.find("assistant") == m_marker.end())
    {
        std::ostringstream e;

        e << "this reads templates that mark turns with <|user|> and "
          << "<|assistant|>, and that one names ";

        if(m_roles.empty()) e << "no roles at all";
        else {
            e << "only";

            for(std::size_t i = 0; i < m_roles.size(); i++)
                e << " <|" << m_roles[i] << "|>";
        }

        e << " -- see chat.hh for what is and is not covered";

        throw exception(e.str());
    }
}

std::string chat::marker(const std::string& role) const {
    std::map<std::string, std::string>::const_iterator i = m_marker.find(role);

    return i == m_marker.end() ? std::string() : i->second;
}

std::string chat::format(const std::vector<message>& turns,
                         bool add_generation_prompt) const
{
    std::string out;

    for(std::size_t i = 0; i < turns.size(); i++) {
        const std::string m = marker(turns[i].role);

        if(m.empty())
            throw exception("the template has no marker for the role '" +
                            turns[i].role + "'");

        // Marker, newline, content, end-of-sequence, newline.  The newlines
        // come from the template's own layout rather than from taste: Jinja
        // emits the one inside the marker literal and the one after the
        // expression that wrote it.
        out += m;
        out += "\n";
        out += turns[i].content;
        out += m_eos;
        out += "\n";
    }

    // And the bare marker, which is the whole point of the exercise: it puts
    // the model at the start of the assistant's turn, so its next token begins
    // a reply instead of continuing the user's sentence.
    if(add_generation_prompt) {
        out += marker("assistant");
        out += "\n";
    }

    return out;
}

}
}
