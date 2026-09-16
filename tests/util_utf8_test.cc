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

// Holding a character until it is whole.
//
// Not for the reason #185 gives.  ncurses accumulates an incomplete multibyte
// sequence across waddstr calls by itself, so a streamed emoji was always
// drawn as one cell -- measured, and written down in jlib/util/utf8.hh.  What
// it cannot do is finish a sequence that never completes: a reply cut short
// mid-character leaves ncurses holding bytes, and the newline jalpaca writes
// next comes out as a literal ^J, so the transcript loses the line break at
// exactly the point a reader needs it.
//
// Holding means ncurses is never given a partial sequence at all.  That half
// -- what reaches the screen -- needs a pty, a terminal emulator and a model,
// and is in the python harness.  This is the rule about where a character
// ends, which is arithmetic and wants no terminal.
//
// It is in jlib/util rather than beside jalpaca because the screen is not the
// only place a partial sequence is a problem: jserve sends one SSE event per
// token, so the same four bytes become four JSON texts that do not parse
// (#249).  A terminal reassembles adjacent bytes and a JSON parser does not,
// which is why one of those was invisible and the other is an exception in
// somebody's client.

#include <jlib/util/utf8.hh>

#include <iostream>
#include <string>
#include <vector>

using jlib::util::utf8_stream;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** A string as hex, so a failure says which bytes rather than drawing them. */
static std::string hex(const std::string& s) {
    static const char* d = "0123456789abcdef";

    std::string out;

    for(char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);

        out += d[c >> 4];
        out += d[c & 0xF];
        out += ' ';
    }

    return out;
}

/** U+1F604, the character from the issue. */
static const std::string SMILE = "\xf0\x9f\x98\x84";

static void a_character_arriving_in_pieces() {
    std::cout << "\nthe four tokens TinyLlama encodes an emoji as:\n";

    utf8_stream s;

    // 29871[20] 243[f0] 162[9f] 155[98] 135[84] -- a space, then the four
    // bytes as four separate byte-fallback tokens.
    ok("the space goes straight through", s.feed(" ") == " ");

    ok("the lead byte draws nothing", s.feed("\xf0").empty());
    ok("  nor the first continuation", s.feed("\x9f").empty());
    ok("  nor the second", s.feed("\x98").empty());

    const std::string last = s.feed("\x84");

    ok("and the last one delivers the whole character", last == SMILE, hex(last));

    ok("  leaving nothing held", s.pending() == 0);
    ok("  and nothing to drop at the end", s.end() == 0);
}

static void a_character_arriving_whole() {
    std::cout << "\na vocabulary that has the character as one token:\n";

    // Gemma 2 has it as 239634[f09f9884], which is why #183's setlocale fix
    // looked like the whole story.
    utf8_stream s;

    ok("passes straight through", s.feed(SMILE) == SMILE);
    ok("  holding nothing", s.pending() == 0);

    ok("and so does a mixed piece",
       s.feed("hi " + SMILE + "!") == "hi " + SMILE + "!");
}

static void a_piece_that_ends_mid_character() {
    std::cout << "\na piece that ends half way through a character:\n";

    utf8_stream s;

    const std::string first = s.feed("ok \xf0\x9f");

    ok("the complete part is drawn", first == "ok ", hex(first));
    ok("  and the rest is held", s.pending() == 2);

    const std::string rest = s.feed("\x98\x84 done");

    ok("the next piece completes it", rest == SMILE + " done", hex(rest));
    ok("  and holds nothing further", s.pending() == 0);
}

static void a_reply_that_stops_mid_character() {
    std::cout << "\na reply that stops mid character:\n";

    // The case that makes end() necessary rather than tidy: ESC during a
    // reply, or a model that runs out of budget between byte-fallback tokens.
    utf8_stream s;

    s.feed("\xf0\x9f\x98");

    ok("three bytes are waiting", s.pending() == 3);

    ok("end() gives them up, and says how many", s.end() == 3);

    ok("  leaving the stream clean for the next reply", s.pending() == 0);

    ok("  which starts again from nothing", s.feed("a") == "a");
}

static void bytes_that_cannot_start_a_character() {
    std::cout << "\nbytes that cannot start a character:\n";

    utf8_stream s;

    // A continuation with no lead.  Drawing it is the bug; holding it forever
    // is the failure this class exists to prevent.
    ok("a stray continuation is dropped", s.feed("\x9f").empty());
    ok("  and not held", s.pending() == 0);

    // C0 and C1 are always overlong, F5 and above exceed U+10FFFF.
    ok("an overlong lead is dropped", s.feed("\xc0").empty() && s.pending() == 0);
    ok("a lead past U+10FFFF is dropped", s.feed("\xf5").empty() && s.pending() == 0);

    ok("and good text on either side still arrives",
       s.feed("a\x9f" "b") == "ab");
}

static void a_lead_byte_that_is_never_finished() {
    std::cout << "\na lead byte interrupted by the next character:\n";

    utf8_stream s;

    // Held bytes that can never complete must not swallow what follows them.
    const std::string out = s.feed("\xf0\x9f" "hello");

    ok("the broken sequence is dropped", out == "hello", hex(out));

    ok("  and the good character that ended it is kept", s.pending() == 0);

    const std::string two = s.feed("\xe2\x98" "\xe2\x98\x83");

    ok("a second lead cancels the first, and itself completes",
       two == "\xe2\x98\x83", hex(two));
}

int main() {
    std::cout << std::unitbuf;

    a_character_arriving_in_pieces();
    a_character_arriving_whole();
    a_piece_that_ends_mid_character();
    a_reply_that_stops_mid_character();
    bytes_that_cannot_start_a_character();
    a_lead_byte_that_is_never_finished();

    // What a green run does not establish.
    //
    // **That anything is drawn correctly.**  Every assertion here is about
    // which bytes come back out; what ncurses then does with them is the half
    // that needs a pty, a terminal emulator and a model.  The ^J that this
    // whole change exists to prevent is invisible from here -- it is in the
    // harness run recorded on #185 and in the branch write-up.
    //
    // Not that the sequences are *valid* UTF-8 beyond their length: an
    // overlong three-byte form or a surrogate passes through here, because
    // where a character ends is the only question being asked.  What produced
    // these bytes is the model's own tokenizer, and ai::pretokenizer has the
    // strict decoder for when the codepoint itself matters.
    //
    // And nothing about combining characters.  A base plus a combining mark is
    // two characters that occupy one cell, which curses handles and this does
    // not participate in -- but it is the next thing that will look like this
    // bug and is not (#176 is the related normalisation question).
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
