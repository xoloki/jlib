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

#include <jlib/ai/tokenizer.hh>

#include <sstream>

namespace jlib {
namespace ai {

namespace {

    /** U+2581 LOWER ONE EIGHTH BLOCK, which stands in for a space. */
    const char* MARKER = "\xe2\x96\x81";

    /** Two hex digits to a byte, for reading a <0xXX> token's name. */
    int hex_pair(const std::string& s, std::size_t at) {
        int v = 0;

        for(int i = 0; i < 2; i++) {
            const char c = s[at + i];

            v <<= 4;

            if(c >= '0' && c <= '9') v |= (c - '0');
            else if(c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
            else if(c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
            else return -1;
        }

        return v;
    }

}

tokenizer::tokenizer(const gguf& g)
    : m_byte_token(256, -1)
{
    if(!g.has("tokenizer.ggml.tokens"))
        throw exception("the file carries no vocabulary");

    m_tokens = g.get("tokenizer.ggml.tokens").strings;

    if(m_tokens.empty())
        throw exception("an empty vocabulary");

    m_types.assign(m_tokens.size(), normal);

    if(g.has("tokenizer.ggml.token_type")) {
        const std::vector<double>& t = g.get("tokenizer.ggml.token_type").numbers;

        if(t.size() != m_tokens.size())
            throw exception("as many token types as tokens, or none");

        for(std::size_t i = 0; i < t.size(); i++)
            m_types[i] = static_cast<type>(int(t[i]));
    }

    for(std::size_t i = 0; i < m_tokens.size(); i++) {
        // First wins.  A vocabulary should not repeat a piece, and if one does
        // the lower id is the one every conversion tool would have used.
        if(m_by_piece.find(m_tokens[i]) == m_by_piece.end())
            m_by_piece[m_tokens[i]] = int(i);

        // <0xXX>, which is how a byte with no token of its own is spelled.
        if(m_types[i] == byte && m_tokens[i].size() == 6 &&
           m_tokens[i].compare(0, 3, "<0x") == 0 && m_tokens[i][5] == '>')
        {
            const int v = hex_pair(m_tokens[i], 3);

            if(v >= 0) m_byte_token[std::size_t(v)] = int(i);
        }
    }

    if(!g.has("tokenizer.ggml.merges"))
        throw exception("the file carries no merge list, and the scores in "
                        "these files are not usable -- see tokenizer.hh");

    const std::vector<std::string>& merges =
        g.get("tokenizer.ggml.merges").strings;

    if(merges.empty())
        throw exception("an empty merge list");

    for(std::size_t i = 0; i < merges.size(); i++) {
        // Stored already in the form the lookup wants, so the split is only to
        // reject a line that has no space in it at all.
        if(merges[i].find(' ') == std::string::npos) {
            std::ostringstream e;

            e << "merge " << i << " has no space in it: '" << merges[i] << "'";

            throw exception(e.str());
        }

        if(m_rank.find(merges[i]) == m_rank.end())
            m_rank[merges[i]] = int(i);
    }

    if(g.has("tokenizer.ggml.bos_token_id"))
        m_bos = int(g.integer("tokenizer.ggml.bos_token_id"));

    if(g.has("tokenizer.ggml.eos_token_id"))
        m_eos = int(g.integer("tokenizer.ggml.eos_token_id"));

    if(g.has("tokenizer.ggml.unknown_token_id"))
        m_unk = int(g.integer("tokenizer.ggml.unknown_token_id"));
}

const std::string& tokenizer::token(int id) const {
    if(id < 0 || std::size_t(id) >= m_tokens.size()) {
        std::ostringstream e;

        e << "token id " << id << " is outside a vocabulary of "
          << m_tokens.size();

        throw exception(e.str());
    }

    return m_tokens[std::size_t(id)];
}

tokenizer::type tokenizer::token_type(int id) const {
    if(id < 0 || std::size_t(id) >= m_types.size())
        throw exception("token type asked for an id outside the vocabulary");

    return m_types[std::size_t(id)];
}

int tokenizer::id_of(const std::string& piece) const {
    std::map<std::string, int>::const_iterator i = m_by_piece.find(piece);

    return i == m_by_piece.end() ? -1 : i->second;
}

std::size_t tokenizer::char_len(const std::string& s, std::size_t at) {
    const unsigned char c = static_cast<unsigned char>(s[at]);

    std::size_t n = 1;

    if((c & 0xE0) == 0xC0) n = 2;
    else if((c & 0xF0) == 0xE0) n = 3;
    else if((c & 0xF8) == 0xF0) n = 4;

    // Truncated or malformed input gets one byte, which the byte fallback then
    // handles.  Refusing here would mean a tokenizer that throws on input it
    // could perfectly well represent.
    if(at + n > s.size()) n = 1;

    return n;
}

std::vector<int> tokenizer::encode(const std::string& text, bool add_bos) const {
    std::vector<int> out;

    if(add_bos && m_bos >= 0) out.push_back(m_bos);

    // Empty in, nothing but the sentence marker out.  Without this the dummy
    // prefix below would be the whole input and encode to a lone space token,
    // which is a word that was never written.
    if(text.empty()) return out;

    // The marker for every space, and one in front: see the header.
    std::string prepared = MARKER;

    for(std::size_t i = 0; i < text.size(); i++) {
        if(text[i] == ' ') prepared += MARKER;
        else prepared += text[i];
    }

    // One symbol per character to begin with, which is what the merge table
    // was built over.
    std::vector<std::string> syms;

    for(std::size_t i = 0; i < prepared.size(); ) {
        const std::size_t n = char_len(prepared, i);

        syms.push_back(prepared.substr(i, n));

        i += n;
    }

    // Merge the best-ranked adjacent pair until none is left.  Quadratic, and
    // the header says why that is allowed to stand.
    for(;;) {
        int best = -1;
        std::size_t at = 0;

        for(std::size_t i = 0; i + 1 < syms.size(); i++) {
            std::map<std::string, int>::const_iterator r =
                m_rank.find(syms[i] + " " + syms[i + 1]);

            if(r != m_rank.end() && (best < 0 || r->second < best)) {
                best = r->second;
                at = i;
            }
        }

        if(best < 0) break;

        syms[at] += syms[at + 1];

        syms.erase(syms.begin() + long(at) + 1);
    }

    for(std::size_t i = 0; i < syms.size(); i++) {
        const int id = id_of(syms[i]);

        if(id >= 0) {
            out.push_back(id);

            continue;
        }

        // No token for this piece, so spell it out in bytes.  Every one of the
        // 256 exists in a Llama vocabulary, so this cannot fail -- but if a
        // vocabulary were missing one, <unk> is the honest answer.
        for(std::size_t b = 0; b < syms[i].size(); b++) {
            const unsigned char c = static_cast<unsigned char>(syms[i][b]);
            const int bt = m_byte_token[c];

            out.push_back(bt >= 0 ? bt : m_unk);
        }
    }

    return out;
}

std::string tokenizer::decode(const std::vector<int>& ids) const {
    std::string out;

    for(std::size_t i = 0; i < ids.size(); i++) {
        const int id = ids[i];

        if(id < 0 || std::size_t(id) >= m_tokens.size())
            throw exception("decode: an id outside the vocabulary");

        // <s> and </s> mark the text rather than appearing in it.
        if(m_types[std::size_t(id)] == control) continue;

        if(m_types[std::size_t(id)] == byte) {
            const int v = hex_pair(m_tokens[std::size_t(id)], 3);

            if(v >= 0) { out += char(v); continue; }
        }

        const std::string& piece = m_tokens[std::size_t(id)];

        for(std::size_t at = 0; at < piece.size(); ) {
            if(piece.compare(at, 3, MARKER) == 0) {
                out += ' ';
                at += 3;
            }
            else {
                out += piece[at];
                at++;
            }
        }
    }

    // And the dummy prefix, which encode() put there and no caller asked for.
    if(!out.empty() && out[0] == ' ') out.erase(0, 1);

    return out;
}

}
}
