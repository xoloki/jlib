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
 */

#include <jlib/util/encoded_word.hh>
#include <jlib/util/rfc2047.hh>
#include <jlib/util/util.hh>

#include <jlib/util/abnf.hh>

#include <algorithm>

namespace jlib {
namespace util {
namespace rfc2047 {

namespace {

using abnf::grammar;
using abnf::match;
using abnf::options;

/** Built on first use, for the reason at crypt/curve.hh:42. */
const grammar& words()
{
    static grammar g = [] {
        grammar g = abnf::compile(ENCODED_WORD);
        g.check();

        return g;
    }();

    return g;
}

options parse_options()
{
    options o;

    o.captures = options::capture_policy::listed;
    o.capture_only = { "charset", "encoding", "encoded-text" };

    return o;
}

bool is_wsp(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/**
 * RFC 2047 4.2: like quoted-printable, but "_" is a space.
 *
 * The underscore rule exists because a space would end the encoded word, and
 * "=20" everywhere is unreadable.  qp::decode does the escapes; this only has
 * to put the spaces back first.
 */
std::string q_decode(std::string_view s, bool& clean)
{
    std::string t(s);

    std::replace(t.begin(), t.end(), '_', ' ');

    return qp::decode(t, clean);
}

/**
 * The one encoded word beginning at i, or an empty span.
 *
 * encoded-text excludes "?" and so do charset and encoding, so the first "?="
 * at or after i+2 is the end of the word and there is nothing to search for.
 * Returns the offset just past it, or npos.
 */
std::size_t word_end(std::string_view s, std::size_t i)
{
    const std::size_t j = s.find("?=", i + 2);

    return j == s.npos ? s.npos : j + 2;
}

/**
 * RFC 2047 5: a word has to be delimited.
 *
 * Whitespace on either side, or the start or end of the field, or the
 * parentheses of a comment.  Without this, "=?" inside an ordinary token --
 * a URL with a query string in it, most often -- starts a decode.
 */
bool delimited(std::string_view s, std::size_t begin, std::size_t end)
{
    // Either bracket on either side: an encoded word may abut the start or
    // the end of a comment, and being stricter than that means real headers
    // come out as their own markup, which is the failure a user actually sees.
    if(begin > 0 && !is_wsp(s[begin - 1]) &&
       s[begin - 1] != '(' && s[begin - 1] != ')') return false;

    if(end < s.size() && !is_wsp(s[end]) &&
       s[end] != ')' && s[end] != '(') return false;

    return true;
}

}

std::string decode(std::string_view s, std::vector<std::string>& charsets)
{
    std::string out;
    std::string pending;          // whitespace not yet committed to out
    bool after_word = false;      // the last thing written was an encoded word

    charsets.clear();
    out.reserve(s.size());

    for(std::size_t i = 0; i < s.size(); ) {
        if(s[i] == '=' && i + 1 < s.size() && s[i + 1] == '?') {
            const std::size_t end = word_end(s, i);

            if(end != s.npos && delimited(s, i, end)) {
                const std::string_view span = s.substr(i, end - i);
                const abnf::parse_result r =
                    words().at("encoded-word").try_parse(span, parse_options());

                if(r) {
                    const std::string enc =
                        upper(r.root()["encoding"].str());
                    const match text = r.root()["encoded-text"];

                    bool known = true;
                    bool clean = true;
                    std::string dec;

                    if(enc == "B")      dec = base64::decode(text.str(), clean);
                    else if(enc == "Q") dec = q_decode(text.text(), clean);
                    else                known = false;

                    if(known) {
                        // RFC 2047 6.2: whitespace between two adjacent
                        // encoded words is not part of the text.  It is held
                        // back rather than written, so that a name folded
                        // across two words comes back without a gap in it.
                        if(!after_word) out += pending;

                        pending.clear();
                        out += dec;
                        charsets.push_back(lower(r.root()["charset"].str()));
                        after_word = true;
                        i = end;

                        continue;
                    }
                }
            }
        }

        // Not an encoded word.  Whitespace is held until something that is not
        // whitespace decides whether it survives.
        if(is_wsp(s[i])) {
            pending += s[i];
        }
        else {
            out += pending;
            pending.clear();
            out += s[i];
            after_word = false;
        }

        i++;
    }

    out += pending;

    return out;
}

std::string decode(std::string_view s)
{
    std::vector<std::string> charsets;

    return decode(s, charsets);
}

namespace {

/** Everything RFC 2047 4.2 lets a Q-encoded word carry unescaped. */
bool q_literal(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '!' || c == '*' || c == '+' || c == '-' || c == '/';
}

bool ascii(std::string_view s)
{
    for(unsigned char c : s) {
        if(c >= 0x80) return false;
    }

    return true;
}

}

std::string encode(std::string_view s, const std::string& charset)
{
    // "=?" + charset + "?B?" + text + "?=" must come to 75 or fewer.
    const std::size_t overhead = charset.size() + 7;
    const std::size_t room = overhead >= 75 ? 4 : 75 - overhead;

    // base64 carries three octets in four characters, so this many octets fit.
    const std::size_t chunk = std::max<std::size_t>(3, room / 4 * 3);

    std::string out;
    std::string held;   // source text waiting to go into an encoded word

    // Chunks of one run, joined with a space that 6.2 takes back out.
    const auto flush = [&] {
        for(std::size_t k = 0; k < held.size(); k += chunk) {
            if(k) out += " ";

            out += "=?" + charset + "?B?" +
                   base64::encode(held.substr(k, chunk)) + "?=";
        }

        held.clear();
    };

    for(std::size_t i = 0; i < s.size(); ) {
        const std::size_t gap_at = i;

        while(i < s.size() && is_wsp(s[i])) i++;

        const std::string_view gap = s.substr(gap_at, i - gap_at);
        const std::size_t word_at = i;

        while(i < s.size() && !is_wsp(s[i])) i++;

        const std::string_view word = s.substr(word_at, i - word_at);

        if(ascii(word)) {
            flush();
            out += gap;
            out += word;

            continue;
        }

        // A whole word at a time.  This used to start the encoded word at the
        // first byte over 0x7F, which produced "Jose N=?utf-8?B?...?=" -- an
        // encoded word with no whitespace before it, which RFC 2047 5 does not
        // allow and no conformant reader will decode.
        //
        // And two adjacent words that both need encoding go into *one* word,
        // gap included, because 6.2 removes whitespace between two encoded
        // words -- so emitting them separately would send "Jose Nunez" and
        // deliver "JoseNunez".
        if(held.empty()) out += gap;
        else             held += gap;

        held += word;
    }

    flush();

    return out;
}
}
}
}
