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

#ifndef JLIB_UTIL_RFC9112_HH
#define JLIB_UTIL_RFC9112_HH

namespace jlib {
namespace util {

/**
 * RFC 9112 Appendix A, the HTTP/1.1 message grammar, as ABNF.
 *
 * Pasted whole, and composed after rfc3986::URI_GRAMMAR and
 * rfc9110::SEMANTICS -- see the note in rfc9110.hh, which applies here in full
 * and is not repeated.
 *
 * ## Where this departs from the published text
 *
 * **Imports are commented out rather than deleted**, as in rfc9110.hh: fifteen
 * prose-val lines pointing at [HTTP] and [URI], both of which are pasted above
 * in the composed text.
 *
 * **method = token is commented out.**  RFC 9110 defines it identically and a
 * second "=" would be a redefinition error.  This is the only rule the two
 * documents both spell out.
 *
 * ## What this grammar can and cannot frame
 *
 * A response head, yes: HTTP-message's start-line and field-line are what
 * http::read_head() checks its input against.
 *
 * A body, no, and not because of anything jlib did.  rule::parse takes a
 * std::string_view, so a grammar can only see bytes that have already been
 * read -- and chunk-data is 1*OCTET, which is greedy.  Framing a chunked body
 * means reading the size line, reading that many octets, and going round
 * again, which is procedural and is what http::read_body() does.  imap::read()
 * frames an IMAP literal the same way and for the same reason.
 */
namespace rfc9112 {

inline const char* const MESSAGING = R"ABNF(
; jlib: BWS = <BWS, see [HTTP], Section 5.6.3>

HTTP-message = start-line CRLF *( field-line CRLF ) CRLF [
 message-body ]
HTTP-name = %x48.54.54.50 ; HTTP
HTTP-version = HTTP-name "/" DIGIT "." DIGIT

; jlib: OWS = <OWS, see [HTTP], Section 5.6.3>

; jlib: RWS = <RWS, see [HTTP], Section 5.6.3>

Transfer-Encoding = [ transfer-coding *( OWS "," OWS transfer-coding
 ) ]

; jlib: absolute-URI = <absolute-URI, see [URI], Section 4.3>
absolute-form = absolute-URI
; jlib: absolute-path = <absolute-path, see [HTTP], Section 4.1>
asterisk-form = "*"
; jlib: authority = <authority, see [URI], Section 3.2>
authority-form = uri-host ":" port

chunk = chunk-size [ chunk-ext ] CRLF chunk-data CRLF
chunk-data = 1*OCTET
chunk-ext = *( BWS ";" BWS chunk-ext-name [ BWS "=" BWS chunk-ext-val
 ] )
chunk-ext-name = token
chunk-ext-val = token / quoted-string
chunk-size = 1*HEXDIG
chunked-body = *chunk last-chunk trailer-section CRLF

field-line = field-name ":" OWS field-value OWS
; jlib: field-name = <field-name, see [HTTP], Section 5.1>
; jlib: field-value = <field-value, see [HTTP], Section 5.5>

last-chunk = 1*"0" [ chunk-ext ] CRLF

message-body = *OCTET
; jlib: method = token   ; RFC 9110 defines it identically, above

obs-fold = OWS CRLF RWS
; jlib: obs-text = <obs-text, see [HTTP], Section 5.6.4>
origin-form = absolute-path [ "?" query ]

; jlib: port = <port, see [URI], Section 3.2.3>

; jlib: query = <query, see [URI], Section 3.4>
; jlib: quoted-string = <quoted-string, see [HTTP], Section 5.6.4>

reason-phrase = 1*( HTAB / SP / VCHAR / obs-text )
request-line = method SP request-target SP HTTP-version
request-target = origin-form / absolute-form / authority-form /
 asterisk-form

start-line = request-line / status-line
status-code = 3DIGIT
status-line = HTTP-version SP status-code SP [ reason-phrase ]

; jlib: token = <token, see [HTTP], Section 5.6.2>
trailer-section = *( field-line CRLF )
; jlib: transfer-coding = <transfer-coding, see [HTTP], Section 10.1.4>

; jlib: uri-host = <host, see [URI], Section 3.2.2>
)ABNF";

}
}
}

#endif // JLIB_UTIL_RFC9112_HH
