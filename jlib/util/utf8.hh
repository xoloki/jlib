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

}
}

#endif // JLIB_UTIL_UTF8_HH
