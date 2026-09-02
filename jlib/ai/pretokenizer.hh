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

#ifndef JLIB_AI_PRETOKENIZER_HH
#define JLIB_AI_PRETOKENIZER_HH

#include <jlib/util/abnf.hh>

#include <exception>
#include <string>
#include <vector>

namespace jlib {
namespace ai {

/**
 * Where a BPE tokenizer is allowed to merge, as a grammar.
 *
 * A byte-level BPE vocabulary does not merge across arbitrary text.  The text
 * is first cut into chunks, and the merge table only ever runs *inside* one --
 * which is why "1234567890" is four tokens and not three, and why "   x" gives
 * its last space to the x rather than keeping it.  Without this the merges run
 * over everything and produce tokens the model was never trained on: not a
 * crash, just a prompt slightly off the distribution, which is the failure
 * this file exists to remove.
 *
 * The cut is specified as a regex, and `tokenizer.ggml.pre` names which one.
 * Two are implemented; `supported()` says which, and `tokenizer` refuses a
 * file naming any other rather than running the wrong pattern over it and
 * calling the ids right.
 *
 * Both are the `Split` step of the file's own pre_tokenizer,
 * `behavior: Isolated`, copied out of the vendor's tokenizer.json.
 *
 * `llama-bpe`, from Llama 3.2:
 *
 *     (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}{1,3}
 *     | ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+
 *
 * `qwen2`, from Qwen 2.5:
 *
 *     (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}
 *     | ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+
 *
 * **They differ in one character.**  Llama takes digits `{1,3}` at a time and
 * Qwen one at a time, so "1234567890" is four chunks under the first and ten
 * under the second, and everything else about the two is the same text.  That
 * is why the grammar below is one body with the `digits` rule prepended per
 * flavour rather than two copies differing in a line -- a second copy would
 * be forty lines that have to be *checked* to find out they agree.
 *
 * ## Why a grammar and not a regex
 *
 * Nothing available can run that pattern.  `util::Regex` is POSIX: no
 * `\p{...}`, no lookahead, and matching bytes rather than codepoints, so
 * `[[:alpha:]]` would test the halves of a UTF-8 sequence.  `std::regex` has
 * the lookahead and still no `\p{...}`.  Reading it by hand is what is left,
 * and this library's answer to reading something by hand is to write down the
 * grammar -- so the seven alternatives are seven rules, in the order the regex
 * gives them, and a reader can check them against the pattern above.
 *
 * ## Where this departs from the pattern
 *
 * Each marked "; jlib:" below.
 *
 * **The character classes are supplied in combinators**, because ABNF has no
 * notion of a Unicode general category and no way to spell one.  See
 * jlib/ai/unicode.hh for the tables and pretokenizer.cc for the predicates.
 * Same shape as RFC 3501's `literal`, which `counted()` supplies for the same
 * reason: the grammar says what the rule *is*, and the combinator says what
 * cannot be written.
 *
 * **`\s+(?!\S)` is one predicate rather than a repetition and a lookahead.**
 * PEG has no negative lookahead here, and the rule's meaning is simpler than
 * its spelling anyway: a run of whitespace gives its last character to
 * whatever follows, unless nothing follows.
 *
 * **Ordered choice is what the regex already meant.**  The reference
 * implementation uses Rust's regex crate, whose alternation is leftmost-first,
 * so trying the branches in order is the specified behaviour rather than an
 * approximation of it.  This is the one place where PEG's usual departure from
 * RFC 5234 is not a departure at all.
 *
 * **A byte that begins no codepoint is its own chunk**, which the pattern has
 * no rule for because it never sees one.  See `stray-byte` below.
 *
 * **Keywords are case-insensitive**, which `(?i:)` asks for on the
 * contractions and ABNF gives for free on every char-val.
 */
namespace pretokenizer {

/**
 * Thrown by split() and by grammar().
 *
 * Its own type rather than backend_error: a caller here is holding a prompt,
 * not a device, and the two want telling apart.  backend.hh was included for
 * nothing else.
 */
class exception : public std::exception {
public:
    exception(const std::string& msg) : m_msg("jlib::ai::pretokenizer: " + msg) {}

    const char* what() const throw() { return m_msg.c_str(); }

private:
    std::string m_msg;
};

/** Whether `tokenizer.ggml.pre` names a pattern this implements. */
bool supported(const std::string& pre);

/**
 * Everything both patterns share, which is all of it but `digits`.
 *
 * Not a grammar on its own: `digits` is referenced here and defined by
 * whichever flavour is prepended, so compiling this text alone fails
 * check() -- deliberately, since there is no such thing as "the"
 * pre-tokenizer.
 */
inline const char* const PATTERN_BODY = R"ABNF(
; The whole input, as the chunks the merges may run inside.
chunks         =  *chunk

; The seven alternatives of the pattern, in its order.  First match wins.
chunk          =  contraction / letters / digits / symbols / newlines
               /  held-space / spaces / stray-byte

; (?i:'s|'t|'re|'ve|'m|'ll|'d) -- longest first, so "'re" is not read as "'r".
contraction    =  "'" ( "re" / "ve" / "ll" / "s" / "t" / "m" / "d" )

; [^\r\n\p{L}\p{N}]?\p{L}+
letters        =  [ lead-char ] 1*letter-char

;  ?[^\s\p{L}\p{N}]+[\r\n]*
symbols        =  [ SP ] 1*symbol-char *eol-char

; jlib: \s*[\r\n]+, supplied in combinators.  Written structurally it reads
; "*space-char 1*eol-char", and that cannot work here: a newline *is*
; whitespace, so the possessive repetition swallows the very characters the
; second half needs.  The regex gets there by backtracking; the rule below
; says the same thing directly -- a run of whitespace, cut after the last
; newline in it.
newlines       =  <whitespace up to and including its last CR or LF>

; \s+(?!\S) -- see the note above on why this is one rule.
held-space     =  <whitespace less its last character, unless it ends the input>

; \s+
spaces         =  1*space-char

; jlib: not in the pattern, and last so that it fires only when nothing else
; does.  The reference runs on text that has already been decoded, so it
; cannot be handed a byte that is not part of a codepoint; encode() can be,
; and the tokenizer's contract is that anything encodes -- a byte with no
; token becomes a byte token.  Throwing here would take that away for the
; byte-level vocabularies only, which is the sort of difference nobody finds
; until a user pastes half a character.
stray-byte     =  <one byte that begins no UTF-8 sequence>

; jlib: every one of these is a single UTF-8 codepoint tested against a
; Unicode general category, which ABNF cannot express.  Supplied in
; combinators; named to avoid the core rules, since DIGIT is 0-9 and these are
; not.
letter-char    =  <one codepoint with general category L>
number-char    =  <one codepoint with general category N>
space-char     =  <one codepoint with the White_Space property>
eol-char       =  <CR or LF>
symbol-char    =  <one codepoint that is none of White_Space, L or N>
lead-char      =  <one codepoint that is none of CR, LF, L or N>
)ABNF";

/**
 * `\p{N}{1,3}` -- three at a time, which is why a long number is several
 * tokens and not one.
 */
inline const char* const LLAMA_BPE_DIGITS =
    "digits         =  1*3number-char\n";

/** `\p{N}` -- one at a time, so every digit is its own chunk. */
inline const char* const QWEN2_DIGITS =
    "digits         =  number-char\n";

/**
 * The compiled grammar for a named pattern, with the classes supplied.
 *
 * @param pre `tokenizer.ggml.pre`, or empty for files old enough to predate
 *            the key -- which are all llama-bpe.
 *
 * Compiled once per flavour and cached; throws for a name supported() would
 * have refused.
 */
const util::abnf::grammar& grammar(const std::string& pre = "");

/**
 * The chunks the merge table may run inside.
 *
 * Every byte of the input appears in exactly one chunk, in order, so
 * concatenating the result gives the input back.
 */
std::vector<std::string> split(const std::string& text,
                               const std::string& pre = "");

}

}
}

#endif // JLIB_AI_PRETOKENIZER_HH
