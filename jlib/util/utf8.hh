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

#ifndef JLIB_UTIL_UTF8_HH
#define JLIB_UTIL_UTF8_HH

#include <string>
#include <string_view>

namespace jlib {
namespace util {

/**
 * Bytes in, whole characters out.
 *
 * A byte-fallback vocabulary encodes anything it does not have as one `<0xNN>`
 * token per byte, so U+1F355 through TinyLlama arrives as **four tokens** and
 * `tokenizer::piece()` returns one byte for each.  jalpaca hands each piece to
 * `waddstr` as it arrives.
 *
 * ## What ncurses already does, which is most of this
 *
 * It holds the incomplete sequence itself.  Measured rather than assumed: four
 * bytes written one per `waddstr`, with a `prefresh` between each, leave the
 * pty **contiguous** --
 *
 *     b'one call per byte: \xf0\x9f\x8d\x95 | all at once: \xf0\x9f\x8d\x95'
 *
 * -- and both render as one cell.  So a streamed emoji was never drawn as four
 * pieces of garbage, which is what #185 was filed saying, and the setlocale fix
 * in #183 really was the whole of that story.
 *
 * ## What does break, and is why this exists
 *
 * A sequence that is **never completed**.  A reply stops mid-character when the
 * context fills or Escape lands between two byte-fallback tokens, and ncurses
 * is then holding one to three bytes with no continuation coming.  It discards
 * them, correctly -- and draws the next byte jalpaca writes, which is the `\n`
 * ending the reply, as a literal `^J`:
 *
 *     old:  🍕🍕🍕^J[1941 tokens, 41.30/s, 2048/     <- and the note wraps
 *           2048 context]
 *
 *     new:  🍕🍕🍕🍕
 *           [2 bytes of an unfinished character dropped]
 *           [2018 tokens, 52.09/s, 2048/2048 context]
 *
 * So the transcript loses a line break exactly when a reply was cut short,
 * which is when the reader most needs to see where it ended.
 *
 * Holding the bytes here means ncurses is never left mid-sequence: what it is
 * given is always whole characters, and what could not be completed is given
 * up at `end()` rather than handed over.
 *
 * ## Why this is in util and not beside the app that needed it first
 *
 * Because the screen is not the only place a partial sequence is a problem.
 * `jserve` sends one SSE event per token, so those same four bytes become four
 * JSON texts that do not parse -- measured, twelve broken events in one reply
 * (#249).  A terminal reassembles adjacent bytes and a JSON parser does not,
 * which is exactly why one of those was invisible for months and the other is
 * an exception in somebody else's client.
 *
 * So the rule belongs where anything can reach it.  It started in
 * `jlib/apps/utf8.hh` and moved here when the second caller turned out to be a
 * wire format rather than a window.
 *
 * ## Not a UTF-8 validator
 *
 * It reads the lead byte for a length and the rest for continuation bits, with
 * the RFC 3629 bounds (no C0/C1, nothing above F4) so a length is never
 * promised for a byte that cannot start a character.  It does **not** reject
 * overlong forms or surrogates in a sequence that is otherwise well-formed --
 * ai::pretokenizer's decoder does, because a grammar cares which codepoint it
 * got.  Here the only question is where a character ends, and what produced
 * these bytes is the model's own tokenizer.
 */
class utf8_stream {
public:
    /** The complete characters in what has arrived so far. */
    std::string feed(const std::string& bytes);

    /**
     * Give up on an incomplete character, and say how many bytes went with it.
     *
     * Called when a reply ends, including when it was interrupted -- which is
     * the case that makes this necessary rather than tidy.
     */
    std::size_t end();

    /** Bytes currently held back.  For a test to look at; nothing else needs it. */
    std::size_t pending() const { return m_held.size(); }

    /**
     * How many bytes a character starting with this one has, or 0 if none can.
     *
     * Public because the same table answers a question a stream does not:
     * whether a finished string ends on a character boundary.  See
     * utf8_ends_mid_character() below.
     */
    static std::size_t promised(unsigned char c);

private:
    std::string m_held;
    std::size_t m_need = 0;
};

inline std::size_t utf8_stream::promised(unsigned char c) {
    if(c < 0x80) return 1;    // ASCII
    if(c < 0xC2) return 0;    // a continuation with no lead, or an overlong two
    if(c < 0xE0) return 2;
    if(c < 0xF0) return 3;
    if(c < 0xF5) return 4;    // F5 and above would exceed U+10FFFF

    return 0;
}

inline std::string utf8_stream::feed(const std::string& bytes) {
    std::string out;

    for(char ch : bytes) {
        const unsigned char c = static_cast<unsigned char>(ch);

        if(!m_held.empty()) {
            if((c & 0xC0) == 0x80) {
                m_held.push_back(ch);

                if(m_held.size() == m_need) {
                    out += m_held;

                    m_held.clear();
                    m_need = 0;
                }

                continue;
            }

            // Not the continuation it was promised, so what is held can never
            // be completed.  Drop it and read this byte as a fresh start --
            // rather than dropping this one, which would lose a good character
            // to a bad one in front of it.
            m_held.clear();
            m_need = 0;
        }

        const std::size_t n = promised(c);

        if(n == 0)
            continue;         // nothing can start here

        if(n == 1) {
            out.push_back(ch);

            continue;
        }

        m_held.push_back(ch);
        m_need = n;
    }

    return out;
}

/**
 * Whether a string stops in the middle of a character.
 *
 * The chunk-boundary question, which is not the same as "is this valid
 * UTF-8": a caller handing over a fragment of a stream needs to know that the
 * fragment can stand alone, and that is decided entirely at its tail.
 *
 * `jserve` needs it because an SSE event carries its content inside a JSON
 * string, and a JSON text that is not valid UTF-8 is not valid JSON (#249).
 * A refusal there is cheap and a client's decode error is not.
 *
 * A tail that is not a boundary at all -- four continuation bytes with no lead
 * in front of them, or a lone continuation -- answers **true** as well. The
 * question being asked is "may this be handed on", and the answer for those is
 * no, whatever else is wrong with them.
 */
inline bool utf8_ends_mid_character(const std::string& s);

/**
 * Is this string well-formed UTF-8?
 *
 * Strict in the three ways that matter: no overlong form, no codepoint in the
 * surrogate range D800-DFFF, nothing past U+10FFFF -- plus the structural
 * checks, a continuation byte with nothing before it and a sequence that runs
 * off the end.  Implemented as a table of ranges on the lead byte, so the
 * second byte's bounds are narrowed where the lead byte constrains them
 * (E0 and ED, F0 and F4) rather than checked afterwards.
 *
 * **This is not utf8_stream.** That class is above, it answers "where does a
 * character end" so ncurses is never handed half a sequence, and it says in its
 * own header that it deliberately accepts overlong forms and surrogates because
 * the only question there is a boundary.  Two different questions, and a caller
 * that wants this one would get a wrong answer from that one.
 *
 * It is also **not** jlib::util::xml's validate_utf8(), which is strict in the
 * same way and then additionally enforces XML 1.0 2.2's Char production -- no
 * NUL, no form feed, no U+FFFE.  A WebSocket text frame may contain any of
 * those (RFC 6455 8.1 asks only that the payload be valid UTF-8), so fusing the
 * two checks the way a document format needs would refuse valid messages.
 *
 * **That makes four UTF-8 implementations in the tree, which is one more than
 * this header's own story asks for, so it is tracked rather than left to drift:
 * #401.** `xml.cc`'s is the one that should fold -- it is this plus the Char
 * walk, and both carry the same lead-byte table. It was not folded in with #399
 * because it reports a byte column through an XML parse error, and changing a
 * conforming parser's diagnostics is a separate branch with a separate test
 * burden. `ai/pretokenizer.cc`'s fourth is not a candidate at all: it decodes to
 * codepoints to drive a grammar, so a bool is not the answer it needs.
 *
 * No position is reported.  A caller that needs to say *where* wants the xml
 * one's error, and a caller that only has to decide whether to fail a
 * connection does not.
 */
inline bool utf8_valid(std::string_view s);

inline std::size_t utf8_stream::end() {
    const std::size_t n = m_held.size();

    m_held.clear();
    m_need = 0;

    return n;
}

inline bool utf8_ends_mid_character(const std::string& s) {
    std::size_t back = 0;

    while(back < 4 && back < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[s.size() - 1 - back]);

        if((c & 0xC0) == 0x80) {
            back++;              // a continuation; the lead is further back

            continue;
        }

        const std::size_t n = utf8_stream::promised(c);

        // n == 0 is a byte that cannot start a character at all, so whatever
        // this tail is, it is not a completed one.
        return n == 0 || n != back + 1;
    }

    // Either the string is empty -- which ends on a boundary, vacuously -- or
    // it is nothing but continuation bytes, which is not a character.
    return !s.empty();
}

inline bool utf8_valid(std::string_view s)
{
    std::size_t i = 0;

    while(i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);

        std::size_t len = 0;
        unsigned char lo = 0x80, hi = 0xBF;   // bounds for the *second* byte

        if(c < 0x80)                    { len = 1; }
        else if(c >= 0xC2 && c <= 0xDF) { len = 2; }
        else if(c == 0xE0)              { len = 3; lo = 0xA0; }
        else if(c == 0xED)              { len = 3; hi = 0x9F; }
        else if(c >= 0xE1 && c <= 0xEF) { len = 3; }
        else if(c == 0xF0)              { len = 4; lo = 0x90; }
        else if(c == 0xF4)              { len = 4; hi = 0x8F; }
        else if(c >= 0xF1 && c <= 0xF3) { len = 4; }
        else {
            // A continuation byte on its own; C0 and C1, which could only ever
            // begin an overlong form; F5 and up, which could only encode
            // something past the end of Unicode.
            return false;
        }

        if(i + len > s.size()) return false;

        for(std::size_t k = 1; k < len; k++) {
            const unsigned char n = static_cast<unsigned char>(s[i + k]);
            const unsigned char low  = (k == 1) ? lo : 0x80;
            const unsigned char high = (k == 1) ? hi : 0xBF;

            if(n < low || n > high) return false;
        }

        i += len;
    }

    return true;
}

}
}

#endif // JLIB_UTIL_UTF8_HH
