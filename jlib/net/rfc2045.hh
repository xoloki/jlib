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

#ifndef JLIB_NET_RFC2045_HH
#define JLIB_NET_RFC2045_HH

namespace jlib {
namespace net {

/**
 * MIME's Content-Type and Content-Disposition, as ABNF.
 *
 * Appended to jlib::net::rfc5322::LEXICAL, because RFC 2045 5.1 is explicit
 * that it is not defining its own lexical layer: "comments are allowed in
 * accordance with RFC 822", and quoted-string is that document's.  So the
 * quoted-string a boundary parameter is written in is literally the same
 * production as the one a display name is written in, and it is written once.
 *
 * See jlib/net/content_type.hh for what reads it.
 *
 * ## Where this departs from the published text
 *
 * Marked with "; jlib:" comments, as rfc5322.hh's are, and the same three
 * kinds: names for spans the RFC leaves unnamed, alternatives reordered for
 * ordered choice, and productions deliberately not carried.
 *
 * RFC 2045 writes its grammar in the RFC 822 style with ":= " and prose
 * exclusions -- token is "any CHAR except SPACE, CTLs, or tspecials" -- which
 * is not ABNF and cannot be pasted.  tchar below is that exclusion worked out
 * as ranges, and it is the one place here where a reader has to take something
 * on trust rather than compare it to the document.  The test spells out every
 * excluded character.
 *
 * ## What this grammar is not
 *
 * Not a check that the type is registered.  "application/x-made-up" parses.
 *
 * Not RFC 2047.  An encoded word in a parameter value comes back as the
 * literal "=?utf-8?q?...?=" it was written as -- which is also what RFC 2231
 * exists to replace, and RFC 2231 *is* here.
 *
 * Not a transcoder.  An RFC 2231 extended value is decoded to octets and the
 * charset it was tagged with is handed back beside them; converting them is
 * somebody else's job and doing it silently would be worse than not doing it.
 */
namespace rfc2045 {

/**
 * Section 5.1, plus RFC 2183's Content-Disposition and RFC 2231's parameters.
 *
 * The two headers are one grammar because they are one shape -- a token, then
 * a semicolon-separated list of attribute=value -- and RFC 2183 3 says so:
 * "the disposition type ... using the parameter syntax of RFC 2045".
 */
inline const char* const CONTENT = R"ABNF(
; RFC 2045 5.1.  The header field name and its colon are not here: a caller
; has a Headers already and passes the value.
media-type      =  [CFWS] mt-type "/" mt-subtype parameters [CFWS]
mt-type         =  token
mt-subtype      =  token

; RFC 2183 2.  Same shape with no subtype.
disposition     =  [CFWS] disp-type parameters [CFWS]
disp-type       =  token

; jlib: the RFC writes *(";" parameter).  A trailing or doubled semicolon with
; nothing after it is not legal and is everywhere, and a parameter list is not
; the place to be the only parser that minds.
parameters      =  *( [CFWS] ";" [CFWS] [ parameter ] )

; RFC 2045 5.1 as extended by RFC 2231 4 and 3: a name may carry a "*n"
; section number, a trailing "*" marking the value as extended, or both.
parameter       =  attribute [section] [extended] [CFWS] "=" [CFWS] value
section         =  "*" 1*DIGIT
extended        =  "*"

; RFC 2231 7.  NOT token: "*" is what separates a parameter name from its
; section number and its extension marker, and "%" and "'" are what an
; extended value is punctuated with, so none of the three can be part of a
; name.  token would match "filename*0*" whole and there would be nothing
; left for [section] and [extended] to find.
attribute       =  1*attribute-char
attribute-char  =  %x21 / %x23-24 / %x26 / %x2B / %x2D-2E / %x30-39 /
                   %x41-5A / %x5E-7E

; jlib: quoted-string first.  It is RFC 5322's, so it carries [CFWS] on both
; sides, and token would otherwise match the empty leading comment's worth of
; nothing and commit.
value           =  quoted-string / token

; RFC 2045 5.1: any CHAR except SPACE, CTLs and tspecials, where tspecials is
;     ( ) < > @ , ; : \ " / [ ] ? =
; jlib: worked out as ranges, because the RFC states it as an exclusion.
token           =  1*tchar
tchar           =  %x21 / %x23-27 / %x2A-2B / %x2D-2E / %x30-39 /
                   %x41-5A / %x5E-7E
)ABNF";

}
}
}

#endif // JLIB_NET_RFC2045_HH
