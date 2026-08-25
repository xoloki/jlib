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

#ifndef JLIB_UTIL_RFC2047_HH
#define JLIB_UTIL_RFC2047_HH

namespace jlib {
namespace util {

/**
 * RFC 2047 encoded words, as ABNF.
 *
 * How a Subject or a display name carries a character that a header field may
 * not: the text is encoded to ASCII and wrapped in a marker naming the charset
 * it came from.
 *
 *     Subject: =?utf-8?B?U2Now7ZuZW4gVGFn?=
 *
 * Read by jlib::util::abnf::compile(); see jlib/util/encoded_word.hh for what
 * uses it.  Self-contained -- unlike rfc2045.hh it does not append to
 * rfc5322::LEXICAL, because an encoded word may not contain whitespace or a
 * comment and so has no use for CFWS.
 *
 * ## Where this departs from the published text
 *
 * Marked with "; jlib:" comments, as the other grammars here are.  RFC 2047
 * writes its productions in the RFC 822 style with prose exclusions, so
 * etchar and etext are those exclusions worked out as ranges -- the same
 * situation as tchar in rfc2045.hh, and the test spells out every excluded
 * character for the same reason.
 *
 * ## What the grammar does not decide
 *
 * Where an encoded word is *allowed* to appear.  RFC 2047 section 5 gives
 * three contexts -- as a token in unstructured text, inside a comment, and as
 * a word in a phrase -- and in each of them the word must be delimited by
 * whitespace or by the enclosing punctuation.  A grammar for the word itself
 * cannot say that; encoded_word.cc checks the delimiters around each match,
 * which is what stops "=?" in the middle of an ordinary token from being read
 * as the start of one.
 *
 * Nor the 75-character limit in section 2.  It is stated as a MUST and real
 * senders exceed it constantly, so it is accepted on the way in and obeyed on
 * the way out.
 */
namespace rfc2047 {

inline const char* const ENCODED_WORD = R"ABNF(
; RFC 2047 2
encoded-word    =  "=?" charset ["*" language] "?" encoding "?" encoded-text "?="
charset         =  etoken
language        =  etoken
encoding        =  etoken

; "any printable ASCII character other than '?' or SPACE"
; jlib: as ranges.  0x3F is "?" and 0x20 is the space.
encoded-text    =  1*etext
etext           =  %x21-3E / %x40-7E

; RFC 2047 2: "any CHAR except SPACE, CTLs, and especials", where
;     especials = "(" / ")" / "<" / ">" / "@" / "," / ";" / ":" / <"> /
;                 "/" / "[" / "]" / "?" / "." / "="
; jlib: as ranges.  Note especials is not RFC 2045's tspecials -- it adds "."
; and it does not exclude "\", so this is not rfc2045.hh's token and cannot
; borrow it.
etoken          =  1*etchar
etchar          =  %x21 / %x23-27 / %x2A-2B / %x2D / %x30-39 / %x41-5A /
                   %x5C / %x5E-7E
)ABNF";

}
}
}

#endif // JLIB_UTIL_RFC2047_HH
