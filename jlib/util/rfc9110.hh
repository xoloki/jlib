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

#ifndef JLIB_UTIL_RFC9110_HH
#define JLIB_UTIL_RFC9110_HH

namespace jlib {
namespace util {

/**
 * RFC 9110 Appendix A, the HTTP semantics grammar, as ABNF.
 *
 * Pasted whole -- all 142 rules, not the dozen the client happens to use.  The
 * point of pasting an RFC is that a reader can check it against the document,
 * and a subset breaks that: you would have to take it on trust that what was
 * left out did not matter.  Unreferenced rules cost nothing at runtime.
 * compile() makes the slots and only check() walks them, once.
 *
 * Composed, not standalone.  Concatenate it after rfc3986::URI_GRAMMAR:
 *
 *     grammar g = abnf::compile(std::string(rfc3986::URI_GRAMMAR) +
 *                               rfc9110::SEMANTICS +
 *                               rfc9112::MESSAGING);
 *
 * ## Where this departs from the published text
 *
 * **The imports from RFC 3986 are commented out, not deleted.**  RFC 9110
 * writes nine of its rules as a prose-val pointing at the URI RFC --
 *
 *     absolute-URI = <absolute-URI, see [URI], Section 4.3>
 *
 * -- and RFC 3986 is pasted immediately above in the composed text, so
 * defining it again here would be a redefinition error.  Each such line is
 * still here with a "; jlib:" in front of it, so a reader sees every published
 * line in its published order and can see exactly what was done to it.
 * uri-host is the one that needed more than commenting: RFC 3986 spells that
 * rule "host", so the import is written out as an alias.
 *
 * **Host is commented out, and it is not an import.**  RFC 5234 2.1 makes
 * rulenames case-insensitive, so RFC 9110's `Host` field is the same
 * rule as RFC 3986's `host` production -- which is why RFC 9110 imports that
 * production under the name `uri-host` rather than under its own.  A composed
 * grammar cannot hold both.  Nothing is lost: a Host field is
 * `uri-host [ ":" port ]`, and RFC 3986's `authority` already accepts exactly
 * that.
 *
 * **field-content is restructured**, and it is the only rule here whose text
 * changed.  As published:
 *
 *     field-content = field-vchar [ 1*( SP / HTAB / field-vchar ) field-vchar ]
 *
 * Under possessive repetition the 1*( ... ) eats the trailing field-vchar that
 * the rule then requires, so the optional group always fails and field-content
 * matches exactly one character.  field-value = *field-content then stops at
 * the first space: a Date: header would read as "Mon,".  The form here --
 * field-vchar *( *( SP / HTAB ) field-vchar ) -- accepts the same language and
 * ends every iteration on a field-vchar, so nothing has to be given back.
 *
 * ## The three rules that are still prose
 *
 * language-range (RFC 4647), Language-Tag (RFC 5646) and mailbox (RFC 5322)
 * are left exactly as the RFC writes them.  jlib has an RFC 5322 mailbox
 * grammar, in jlib/net/rfc5322.hh, and it is deliberately *not* composed in:
 * RFC 5322 and RFC 9110 both define comment, ctext, quoted-pair and
 * quoted-string and they do not agree, so pasting one over the other would
 * quietly change the meaning of four rules to get one.
 *
 * So compile() leaves them as prose, grammar::prose_rules() reports them, and
 * a parse that reaches one fails with a position naming the rule.  Inventing a
 * grammar for them would look complete and be wrong, which is worse.
 *
 * ## What a compiling grammar does not prove
 *
 * check() finds undefined references, left recursion, and repetition of
 * something nullable.  It does **not** find an alternation that commits to the
 * wrong branch -- which is the failure mode every grammar in this tree has
 * actually had.  dec-octet, IPv6address, RFC 5234's own defined-as and repeat,
 * five of RFC 5322's, and field-content above were all found by *running*
 * them, never by reading them.
 *
 * So the rules this code exercises are right because they are tested, and the
 * rest are well-formed and unverified.  What is exercised, as of this branch:
 * status-line, HTTP-version, status-code, reason-phrase, field-line,
 * field-name, field-value, token, OWS, and the RFC 3986 rules underneath
 * absolute-URI and URI-reference.
 *
 * Two known-wrong-under-PEG rules that nothing here exercises, written down so
 * the next caller does not have to find them again:
 *
 * - credentials and challenge put token68 before the auth-param list, and
 *   token68 ends in *"=", so `Digest username="x"` matches token68 as
 *   `username=` and the rest is left over.  Whatever implements #104 will have
 *   to reorder those two.
 * - chunk has chunk-data = 1*OCTET, which is greedy and consumes the rest of
 *   the input, so chunked-body cannot frame anything.  That is expected:
 *   chunked framing is procedural, exactly as imap::read() frames a literal.
 */
namespace rfc9110 {

inline const char* const SEMANTICS = R"ABNF(
Accept = [ ( media-range [ weight ] ) *( OWS "," OWS ( media-range [
 weight ] ) ) ]
Accept-Charset = [ ( ( token / "*" ) [ weight ] ) *( OWS "," OWS ( (
 token / "*" ) [ weight ] ) ) ]
Accept-Encoding = [ ( codings [ weight ] ) *( OWS "," OWS ( codings [
 weight ] ) ) ]
Accept-Language = [ ( language-range [ weight ] ) *( OWS "," OWS (
 language-range [ weight ] ) ) ]
Accept-Ranges = acceptable-ranges
Allow = [ method *( OWS "," OWS method ) ]
Authentication-Info = [ auth-param *( OWS "," OWS auth-param ) ]
Authorization = credentials

BWS = OWS

Connection = [ connection-option *( OWS "," OWS connection-option )
 ]
Content-Encoding = [ content-coding *( OWS "," OWS content-coding )
 ]
Content-Language = [ language-tag *( OWS "," OWS language-tag ) ]
Content-Length = 1*DIGIT
Content-Location = absolute-URI / partial-URI
Content-Range = range-unit SP ( range-resp / unsatisfied-range )
Content-Type = media-type

Date = HTTP-date

ETag = entity-tag
Expect = [ expectation *( OWS "," OWS expectation ) ]

From = mailbox

GMT = %x47.4D.54 ; GMT

HTTP-date = IMF-fixdate / obs-date
; jlib: Host = uri-host [ ":" port ]
;
; Commented out, and this one is not an import.  RFC 5234 2.1 makes
; rulenames case-insensitive, so RFC 9110's Host field is the same rule
; as RFC 3986's host production -- which is exactly why RFC 9110 imports
; that production under the name uri-host rather than its own.  A
; composed grammar cannot hold both, and the URI one is the one
; everything else here reaches through.  Nothing is lost: a Host field
; is uri-host [ ":" port ], and RFC 3986's authority already accepts
; precisely that and a userinfo besides.

IMF-fixdate = day-name "," SP date1 SP time-of-day SP GMT
If-Match = "*" / [ entity-tag *( OWS "," OWS entity-tag ) ]
If-Modified-Since = HTTP-date
If-None-Match = "*" / [ entity-tag *( OWS "," OWS entity-tag ) ]
If-Range = entity-tag / HTTP-date
If-Unmodified-Since = HTTP-date

Last-Modified = HTTP-date
Location = URI-reference

Max-Forwards = 1*DIGIT

OWS = *( SP / HTAB )

Proxy-Authenticate = [ challenge *( OWS "," OWS challenge ) ]
Proxy-Authentication-Info = [ auth-param *( OWS "," OWS auth-param )
 ]
Proxy-Authorization = credentials

RWS = 1*( SP / HTAB )
Range = ranges-specifier
Referer = absolute-URI / partial-URI
Retry-After = HTTP-date / delay-seconds

Server = product *( RWS ( product / comment ) )

TE = [ t-codings *( OWS "," OWS t-codings ) ]
Trailer = [ field-name *( OWS "," OWS field-name ) ]

; jlib: URI-reference = <URI-reference, see [URI], Section 4.1>
Upgrade = [ protocol *( OWS "," OWS protocol ) ]
User-Agent = product *( RWS ( product / comment ) )

Vary = [ ( "*" / field-name ) *( OWS "," OWS ( "*" / field-name ) )
 ]
Via = [ ( received-protocol RWS received-by [ RWS comment ] ) *( OWS
 "," OWS ( received-protocol RWS received-by [ RWS comment ] ) ) ]

WWW-Authenticate = [ challenge *( OWS "," OWS challenge ) ]

; jlib: absolute-URI = <absolute-URI, see [URI], Section 4.3>
absolute-path = 1*( "/" segment )
acceptable-ranges = range-unit *( OWS "," OWS range-unit )
asctime-date = day-name SP date3 SP time-of-day SP year
auth-param = token BWS "=" BWS ( token / quoted-string )
auth-scheme = token
; jlib: authority = <authority, see [URI], Section 3.2>

challenge = auth-scheme [ 1*SP ( token68 / [ auth-param *( OWS ","
 OWS auth-param ) ] ) ]
codings = content-coding / "identity" / "*"
comment = "(" *( ctext / quoted-pair / comment ) ")"
complete-length = 1*DIGIT
connection-option = token
content-coding = token
credentials = auth-scheme [ 1*SP ( token68 / [ auth-param *( OWS ","
 OWS auth-param ) ] ) ]
ctext = HTAB / SP / %x21-27 ; '!'-'''
 / %x2A-5B ; '*'-'['
 / %x5D-7E ; ']'-'~'
 / obs-text

date1 = day SP month SP year
date2 = day "-" month "-" 2DIGIT
date3 = month SP ( 2DIGIT / ( SP DIGIT ) )
day = 2DIGIT
day-name = %x4D.6F.6E ; Mon
 / %x54.75.65 ; Tue
 / %x57.65.64 ; Wed
 / %x54.68.75 ; Thu
 / %x46.72.69 ; Fri
 / %x53.61.74 ; Sat
 / %x53.75.6E ; Sun
day-name-l = %x4D.6F.6E.64.61.79 ; Monday
 / %x54.75.65.73.64.61.79 ; Tuesday
 / %x57.65.64.6E.65.73.64.61.79 ; Wednesday
 / %x54.68.75.72.73.64.61.79 ; Thursday
 / %x46.72.69.64.61.79 ; Friday
 / %x53.61.74.75.72.64.61.79 ; Saturday
 / %x53.75.6E.64.61.79 ; Sunday
delay-seconds = 1*DIGIT

entity-tag = [ weak ] opaque-tag
etagc = "!" / %x23-7E ; '#'-'~'
 / obs-text
expectation = token [ "=" ( token / quoted-string ) parameters ]

; jlib: field-content = field-vchar [ 1*( SP / HTAB / field-vchar )
;                       field-vchar ]
;
; The published rule cannot work under possessive repetition: the
; 1*( SP / HTAB / field-vchar ) eats the trailing field-vchar the rule
; then requires, so the optional group always fails and field-content
; matches one character.  field-value = *field-content then stops at
; the first space -- a Date: would read as "Mon,".  Same language,
; written so that every iteration ends on a field-vchar:
field-content = field-vchar *( *( SP / HTAB ) field-vchar )
field-name = token
field-value = *field-content
field-vchar = VCHAR / obs-text
first-pos = 1*DIGIT

hour = 2DIGIT
http-URI = "http://" authority path-abempty [ "?" query ]
https-URI = "https://" authority path-abempty [ "?" query ]

incl-range = first-pos "-" last-pos
int-range = first-pos "-" [ last-pos ]

language-range = <language-range, see [RFC4647], Section 2.1>
language-tag = <Language-Tag, see [RFC5646], Section 2.1>
last-pos = 1*DIGIT

mailbox = <mailbox, see [RFC5322], Section 3.4>
media-range = ( "*/*" / ( type "/*" ) / ( type "/" subtype ) )
 parameters
media-type = type "/" subtype parameters
method = token
minute = 2DIGIT
month = %x4A.61.6E ; Jan
 / %x46.65.62 ; Feb
 / %x4D.61.72 ; Mar
 / %x41.70.72 ; Apr
 / %x4D.61.79 ; May
 / %x4A.75.6E ; Jun
 / %x4A.75.6C ; Jul
 / %x41.75.67 ; Aug
 / %x53.65.70 ; Sep
 / %x4F.63.74 ; Oct
 / %x4E.6F.76 ; Nov
 / %x44.65.63 ; Dec

obs-date = rfc850-date / asctime-date
obs-text = %x80-FF
opaque-tag = DQUOTE *etagc DQUOTE
other-range = 1*( %x21-2B ; '!'-'+'
 / %x2D-7E ; '-'-'~'
 )

parameter = parameter-name "=" parameter-value
parameter-name = token
parameter-value = ( token / quoted-string )
parameters = *( OWS ";" OWS [ parameter ] )
partial-URI = relative-part [ "?" query ]
; jlib: path-abempty = <path-abempty, see [URI], Section 3.3>
; jlib: port = <port, see [URI], Section 3.2.3>
product = token [ "/" product-version ]
product-version = token
protocol = protocol-name [ "/" protocol-version ]
protocol-name = token
protocol-version = token
pseudonym = token

qdtext = HTAB / SP / "!" / %x23-5B ; '#'-'['
 / %x5D-7E ; ']'-'~'
 / obs-text
; jlib: query = <query, see [URI], Section 3.4>
quoted-pair = "\" ( HTAB / SP / VCHAR / obs-text )
quoted-string = DQUOTE *( qdtext / quoted-pair ) DQUOTE
qvalue = ( "0" [ "." *3DIGIT ] ) / ( "1" [ "." *3"0" ] )

range-resp = incl-range "/" ( complete-length / "*" )
range-set = range-spec *( OWS "," OWS range-spec )
range-spec = int-range / suffix-range / other-range
range-unit = token
ranges-specifier = range-unit "=" range-set
received-by = pseudonym [ ":" port ]
received-protocol = [ protocol-name "/" ] protocol-version
; jlib: relative-part = <relative-part, see [URI], Section 4.2>
rfc850-date = day-name-l "," SP date2 SP time-of-day SP GMT

second = 2DIGIT
; jlib: segment = <segment, see [URI], Section 3.3>
subtype = token
suffix-length = 1*DIGIT
suffix-range = "-" suffix-length

t-codings = "trailers" / ( transfer-coding [ weight ] )
tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." /
 "^" / "_" / "`" / "|" / "~" / DIGIT / ALPHA
time-of-day = hour ":" minute ":" second
token = 1*tchar
token68 = 1*( ALPHA / DIGIT / "-" / "." / "_" / "~" / "+" / "/" )
 *"="
transfer-coding = token *( OWS ";" OWS transfer-parameter )
transfer-parameter = token BWS "=" BWS ( token / quoted-string )
type = token

unsatisfied-range = "*/" complete-length
; jlib: uri-host = <host, see [URI], Section 3.2.2>
uri-host      = host   ; jlib: that import, written out --
                       ; RFC 3986 spells the rule "host"

weak = %x57.2F ; W/
weight = OWS ";" OWS "q=" qvalue

year = 4DIGIT
)ABNF";

}
}
}

#endif // JLIB_UTIL_RFC9110_HH
