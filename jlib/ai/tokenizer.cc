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
#include <jlib/ai/pretokenizer.hh>

#include <sstream>

namespace jlib {
namespace ai {

namespace {

    /** U+2581 LOWER ONE EIGHTH BLOCK, which stands in for a space. */
    const char* MARKER = "\xe2\x96\x81";

    /** A codepoint as UTF-8.  Nothing here exceeds U+0143, so two bytes do. */
    std::string utf8(int cp) {
        std::string out;

        if(cp < 0x80) out += char(cp);
        else {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        }

        return out;
    }

    /**
     * GPT-2's byte-to-character map.
     *
     * The printable ASCII range and two runs of Latin-1 stand for themselves;
     * the other 68 bytes -- the control characters, space, and the gaps -- are
     * moved to U+0100 upward, in byte order.  The point is that every byte
     * becomes a printable character, so a vocabulary of characters can spell
     * any input at all and there is no such thing as an unencodable byte.
     *
     * This is why a space appears as U+0120 in these vocabularies: 0x20 is not
     * in any of the direct runs, and it is the first byte that is not.
     */
    std::vector<std::string> byte_chars() {
        std::vector<std::string> out(256);
        int n = 0;

        for(int b = 0; b < 256; b++) {
            const bool direct = (b >= 0x21 && b <= 0x7E) ||
                                (b >= 0xA1 && b <= 0xAC) ||
                                (b >= 0xAE && b <= 0xFF);

            out[std::size_t(b)] = utf8(direct ? b : 256 + n++);
        }

        return out;
    }

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

    // Which convention, from the file rather than from the architecture --
    // see the note on flavour in the header.  "gpt2" is byte-level; "llama"
    // and anything else is treated as SentencePiece, which is what the reader
    // did before there was a choice.
    if(g.has("tokenizer.ggml.model") && g.str("tokenizer.ggml.model") == "gpt2") {
        m_flavour = flavour::byte_level;
        m_byte_char = byte_chars();

        for(std::size_t b = 0; b < m_byte_char.size(); b++)
            m_char_byte[m_byte_char[b]] = static_cast<unsigned char>(b);

        // Which cut, from the file as well.  A byte-level vocabulary is only
        // half the convention: the merges run inside chunks, and a file that
        // names a different pattern would be tokenized by the wrong one --
        // producing ids that are plausible, off-distribution and silent.
        // Refusing is the only honest answer until that pattern is written.
        m_pre = g.has("tokenizer.ggml.pre") ? g.str("tokenizer.ggml.pre") : "";

        if(!pretokenizer::supported(m_pre))
            throw exception("this file's pre-tokenizer is '" + m_pre +
                            "', and the ones implemented are 'llama-bpe' and "
                            "'qwen2' -- see pretokenizer.hh");
    }

    // Whether a prompt should begin with the beginning-of-sequence token, and
    // the file is what says.  Qwen 2.5 says no -- its template opens with
    // <|im_start|>system and prepending <|endoftext|> puts an end-of-document
    // marker in front of every conversation.  Llama and TinyLlama do not
    // carry the key and want one, so absent means yes.
    m_adds_bos = !g.has("tokenizer.ggml.add_bos_token") ||
                 g.integer("tokenizer.ggml.add_bos_token") != 0;

    // The dummy prefix, and the file decides that too.  Gemma 2 sets it to 0:
    // its vocabulary has both "Hello" and "\u2581Hello", and it means the
    // unmarked one for text that begins a prompt.  Absent means yes, which is
    // what Llama and TinyLlama are.
    m_adds_space = !g.has("tokenizer.ggml.add_space_prefix") ||
                   g.integer("tokenizer.ggml.add_space_prefix") != 0;

    // Which signal this file carries.  Scores count only when they are not
    // all zero: TinyLlama's are 128000 zero bytes, and merging on them would
    // tie every candidate and pick by whatever the tie-break happened to be.
    if(g.has("tokenizer.ggml.scores")) {
        const std::vector<double>& sc = g.get("tokenizer.ggml.scores").numbers;

        bool any = false;

        for(std::size_t i = 0; i < sc.size() && !any; i++)
            if(sc[i] != 0) any = true;

        if(any) {
            m_scores.reserve(sc.size());

            for(std::size_t i = 0; i < sc.size(); i++)
                m_scores.push_back(float(sc[i]));
        }
    }

    const bool has_merges = g.has("tokenizer.ggml.merges") &&
                            !g.get("tokenizer.ggml.merges").strings.empty();

    // Merges first when a file has both: they are the pair table the
    // vocabulary was built with, where a score is a property of one token.
    m_driver = has_merges ? driver::merges : driver::scores;

    if(!has_merges && m_scores.size() != m_tokens.size())
        throw exception("this file carries neither a merge list nor a score "
                        "for every token, so there is nothing to merge by -- "
                        "see tokenizer.hh");

    // Under scores there is nothing more to build: a score is indexed by id,
    // and id_of() already finds the token a pair would make.
    if(m_driver == driver::merges) {
        const std::vector<std::string>& merges =
            g.get("tokenizer.ggml.merges").strings;

        for(std::size_t i = 0; i < merges.size(); i++) {
            // Stored already in the form the lookup wants, so the split is
            // only to reject a line that has no space in it at all.
            if(merges[i].find(' ') == std::string::npos) {
                std::ostringstream e;

                e << "merge " << i << " has no space in it: '" << merges[i]
                  << "'";

                throw exception(e.str());
            }

            if(m_rank.find(merges[i]) == m_rank.end())
                m_rank[merges[i]] = int(i);
        }
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

std::vector<int> tokenizer::encode(const std::string& text, bool add_bos,
                                   bool parse_special) const
{
    std::vector<int> out;

    if(add_bos && m_bos >= 0) out.push_back(m_bos);

    // Empty in, nothing but the sentence marker out.  Without this the dummy
    // prefix would be the whole input and encode to a lone space token, which
    // is a word that was never written.
    if(text.empty()) return out;

    append(text, true, parse_special, out);

    return out;
}

void tokenizer::append(const std::string& text, bool add_prefix,
                       bool parse_special, std::vector<int>& out) const
{
    if(text.empty()) return;

    if(!parse_special) {
        encode_run(text, add_prefix, out);

        return;
    }

    // Walk the text, splitting at every control token written out in it, so
    // "Hi</s>" is two tokens and not five.  Only the run that begins the input
    // gets the dummy prefix.
    std::size_t at = 0;
    bool first = add_prefix;

    while(at < text.size()) {
        std::size_t where = std::string::npos;
        int which = -1;

        for(std::size_t i = 0; i < m_tokens.size(); i++) {
            if(m_types[i] != control && m_types[i] != user_defined) continue;

            if(m_tokens[i].empty()) continue;

            const std::size_t found = text.find(m_tokens[i], at);

            // Earliest wins, and the longest of those, so a vocabulary holding
            // both "<|end|>" and "<|end|>!" cannot have the shorter eat the
            // start of the longer.
            if(found != std::string::npos &&
               (found < where ||
                (found == where &&
                 m_tokens[i].size() > m_tokens[std::size_t(which)].size())))
            {
                where = found;
                which = int(i);
            }
        }

        if(which < 0) {
            encode_run(text.substr(at), first, out);

            return;
        }

        if(where > at) {
            encode_run(text.substr(at, where - at), first, out);

            first = false;
        }

        out.push_back(which);

        first = false;
        at = where + m_tokens[std::size_t(which)].size();
    }
}

void tokenizer::encode_run(const std::string& text, bool add_prefix,
                           std::vector<int>& out) const
{
    if(text.empty()) return;

    if(m_flavour == flavour::byte_level) {
        // The merges may not run across a chunk boundary -- that is the whole
        // point of the pre-tokenizer, and why "1234567890" is four tokens
        // rather than three.  Each chunk is mapped and merged on its own.
        const std::vector<std::string> chunks =
            pretokenizer::split(text, m_pre);

        for(std::size_t c = 0; c < chunks.size(); c++) {
            std::string prepared;

            // Byte-mapped *after* the split, which is the order the reference
            // tokenizer uses: its pre_tokenizer is Split then ByteLevel, and
            // the ByteLevel step has use_regex false because the splitting is
            // already done.
            for(std::size_t i = 0; i < chunks[c].size(); i++)
                prepared += m_byte_char[
                    static_cast<unsigned char>(chunks[c][i])];

            merge_run(prepared, out);
        }

        return;
    }

    // The marker for every space, and one in front: see the header.
    std::string prepared = (add_prefix && m_adds_space) ? MARKER : "";

    for(std::size_t i = 0; i < text.size(); i++) {
        if(text[i] == ' ') prepared += MARKER;
        else prepared += text[i];
    }

    merge_run(prepared, out);
}

bool tokenizer::merge_key(const std::string& left, const std::string& right,
                          double& key) const
{
    if(m_driver == driver::merges) {
        const std::map<std::string, int>::const_iterator r =
            m_rank.find(left + " " + right);

        if(r == m_rank.end()) return false;

        key = double(r->second);

        return true;
    }

    // SentencePiece's rule: the pair that makes the highest-scoring token.
    // Negated because this compares lower-is-better, so that the caller does
    // not have to know which driver it is looking at.
    const int id = id_of(left + right);

    if(id < 0) return false;

    key = -double(m_scores[std::size_t(id)]);

    return true;
}

/** The merge table, run over one prepared run of text. */
void tokenizer::merge_run(const std::string& prepared,
                          std::vector<int>& out) const
{
    // One symbol per character to begin with, which is what the merge table
    // was built over.
    std::vector<std::string> syms;

    for(std::size_t i = 0; i < prepared.size(); ) {
        const std::size_t n = char_len(prepared, i);

        syms.push_back(prepared.substr(i, n));

        i += n;
    }

    // Merge the best adjacent pair until none is left -- best by rank or by
    // score, which merge_key is the only place that knows.  Quadratic, and the
    // header says why that is allowed to stand.
    for(;;) {
        bool found = false;
        double best = 0;
        std::size_t at = 0;

        for(std::size_t i = 0; i + 1 < syms.size(); i++) {
            double key = 0;

            if(!merge_key(syms[i], syms[i + 1], key)) continue;

            if(!found || key < best) {
                found = true;
                best = key;
                at = i;
            }
        }

        if(!found) break;

        syms[at] += syms[at + 1];

        syms.erase(syms.begin() + long(at) + 1);
    }

    for(std::size_t i = 0; i < syms.size(); i++) {
        const int id = id_of(syms[i]);

        if(id >= 0) {
            out.push_back(id);

            continue;
        }

        if(m_flavour == flavour::byte_level) {
            // Every mapped character is in the vocabulary by construction, so
            // a symbol with no id means the merges produced something the
            // vocabulary does not contain -- a broken file, not a rare input.
            // Splitting it into characters is the honest recovery; there is no
            // <unk> in these vocabularies to fall back to.
            for(std::size_t at = 0; at < syms[i].size(); ) {
                const std::size_t n = char_len(syms[i], at);
                const int id2 = id_of(syms[i].substr(at, n));

                if(id2 < 0)
                    throw exception("this vocabulary has no token for a "
                                    "character its own merges produced");

                out.push_back(id2);

                at += n;
            }

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
}

/**
 * A byte_level token's characters, back to the bytes they stand for.
 *
 * The inverse of the map applied in encode_run: each character in the token
 * is one byte of the original text.  A character outside the map is passed
 * through, which cannot happen for a well-formed vocabulary and is not worth
 * throwing over if it does.
 */
std::string tokenizer::unmap(const std::string& t) const {
    std::string out;

    for(std::size_t at = 0; at < t.size(); ) {
        const std::size_t n = char_len(t, at);
        const std::map<std::string, unsigned char>::const_iterator i =
            m_char_byte.find(t.substr(at, n));

        if(i != m_char_byte.end()) out += char(i->second);
        else out += t.substr(at, n);

        at += n;
    }

    return out;
}

std::string tokenizer::piece(int id) const {
    if(id < 0 || std::size_t(id) >= m_tokens.size())
        throw exception("piece: an id outside the vocabulary");

    if(m_types[std::size_t(id)] == control) return std::string();

    if(m_flavour == flavour::byte_level) return unmap(m_tokens[std::size_t(id)]);

    if(m_types[std::size_t(id)] == byte) {
        const int v = hex_pair(m_tokens[std::size_t(id)], 3);

        if(v >= 0) return std::string(1, char(v));
    }

    const std::string& t = m_tokens[std::size_t(id)];

    std::string out;

    for(std::size_t at = 0; at < t.size(); ) {
        if(t.compare(at, 3, MARKER) == 0) { out += ' '; at += 3; }
        else { out += t[at]; at++; }
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

        if(m_flavour == flavour::byte_level) {
            out += unmap(m_tokens[std::size_t(id)]);

            continue;
        }

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
    // Only SentencePiece has one; a byte_level leading space is a space the
    // caller wrote, and stripping it would lose a character.
    // Only when encode() put one there.  Stripping it on a file that adds no
    // prefix would eat a space the text actually had.
    if(m_flavour == flavour::sentencepiece && m_adds_space &&
       !out.empty() && out[0] == ' ')
        out.erase(0, 1);

    return out;
}

}
}
