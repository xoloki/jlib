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

#ifndef JLIB_UTIL_RFC5322_HH
#define JLIB_UTIL_RFC5322_HH

namespace jlib {
namespace util {

/**
 * RFC 5322 section 3.2, the lexical tokens the mail RFCs share.
 *
 * Here rather than in jlib/net because three grammars need them and only one
 * of the three is about addresses.  RFC 2045 5.1 says its Content-Type
 * parameters use "the parameter syntax of RFC 822", RFC 2047 2 builds an
 * encoded word out of the same tokens, and RFC 5322 3.4 uses them for
 * addresses -- so the quoted-string a boundary is written in, the one an
 * encoded word's charset may be written in, and the one a display name is
 * written in are one production, written once.
 *
 * Appended to by:
 *
 *     jlib/util/rfc2045.hh    Content-Type and Content-Disposition
 *     jlib/util/rfc2047.hh    encoded words
 *     jlib/net/rfc5322.hh     addresses
 *
 * See jlib/util/abnf.hh for what reads it, and jlib/net/rfc5322.hh for the
 * house rules on marking a departure from a published grammar.
 */
namespace rfc5322 {

inline const char* const LEXICAL = R"ABNF(
; 3.2.1 quoted characters
quoted-pair     =  "\" (VCHAR / WSP)

; 3.2.2 folding white space and comments
FWS             =  [*WSP CRLF] 1*WSP
ctext           =  %d33-39 / %d42-91 / %d93-126
ccontent        =  ctext / quoted-pair / comment
comment         =  "(" *([FWS] ccontent) [FWS] ")"
CFWS            =  (1*([FWS] comment) [FWS]) / FWS

; 3.2.3 atom
atext           =  ALPHA / DIGIT / "!" / "#" / "$" / "%" / "&" / "'" /
                   "*" / "+" / "-" / "/" / "=" / "?" / "^" / "_" / "`" /
                   "{" / "|" / "}" / "~"
atom            =  [CFWS] atom-text [CFWS]
atom-text       =  1*atext                    ; jlib: names the value inside
                                              ; the CFWS, which the span
                                              ; otherwise includes
dot-atom        =  [CFWS] dot-atom-text [CFWS]
dot-atom-text   =  atom-text *("." atom-text) ; jlib: 1*atext respelled, so one
                                              ; extractor serves both policies

; 3.2.4 quoted strings
qtext           =  %d33 / %d35-91 / %d93-126
qcontent        =  qtext / quoted-pair
quoted-string   =  [CFWS] DQUOTE qs-body DQUOTE [CFWS]
qs-body         =  *([FWS] qcontent) [FWS]    ; jlib: names what is between the
                                              ; quotes, which 3.2.4 says is the
                                              ; whole of the value -- so the
                                              ; trailing FWS is inside it, or a
                                              ; parameter written as "a long "
                                              ; loses its last space

)ABNF";

}
}
}

#endif // JLIB_UTIL_RFC5322_HH
