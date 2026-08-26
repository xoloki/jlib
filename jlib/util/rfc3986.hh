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

#ifndef JLIB_UTIL_RFC3986_HH
#define JLIB_UTIL_RFC3986_HH

namespace jlib {
namespace util {

/**
 * RFC 3986 Appendix A, the URI grammar, as ABNF.
 *
 * Pasted from the RFC, which prints it as ABNF already -- so unlike RFC 2045
 * and RFC 2047 there was nothing to translate out of prose, and a reader can
 * check this against Appendix A line by line.
 *
 * Read by jlib::util::abnf::compile(); see jlib/util/URL.hh for what uses it.
 * Self-contained: a URI has no comments and no folding, so nothing here
 * borrows rfc5322::LEXICAL.
 *
 * ## Where this departs from the published text
 *
 * Three places, each marked "; jlib:".
 *
 * **dec-octet is reversed.**  As published it reads
 *
 *     dec-octet = DIGIT / %x31-39 DIGIT / "1" 2DIGIT
 *               / "2" %x30-34 DIGIT / "25" %x30-35
 *
 * and under ordered choice the bare DIGIT matches the "2" of "255" and wins,
 * leaving "55" for a "." that is not there.  Longest first.
 *
 * **host drops IPv4address.**  The RFC writes
 *
 *     host = IP-literal / IPv4address / reg-name
 *
 * but reg-name is *( unreserved / pct-encoded / sub-delims ) and unreserved
 * includes DIGIT and ".", so reg-name already matches every IPv4address and
 * a great deal more.  Leaving IPv4address in the middle makes "1.2.3.4.5" --
 * a perfectly good reg-name -- fail, because IPv4address matches "1.2.3.4",
 * commits, and strands ".5".  The rule is still here, unreferenced, so that a
 * caller holding a host can ask whether it is a dotted quad.
 *
 * **IPv6address is restructured**, and this is the one place a reader cannot
 * check the grammar against Appendix A line by line.  The RFC writes nine
 * alternatives, each counting how many h16 groups sit either side of the "::":
 *
 *     / [ *5( h16 ":" ) h16 ] "::" h16
 *
 * Under possessive repetition that cannot work.  Given "2001:db8::1", the
 * "*5( h16 ":" )" matches "2001:" and then "db8:" -- taking the first colon of
 * the "::" -- and then h16 must match ":" and fails.  The repetition does not
 * give the colon back, so the alternative fails, and so does every other one.
 * ABNF has no way to write "not followed by another colon", so there is no
 * faithful transcription that works.
 *
 * What is here instead is "an optional run of groups, then '::', then an
 * optional run" -- the same shape, without the counting.  It accepts
 * everything RFC 3986 does and some things it does not: more than eight
 * groups, an IPv4address somewhere other than the end, and more than one of
 * them.  That is the right trade for a URI parser, whose job is to find where
 * the host ends; whether the characters between the brackets are an address
 * is getaddrinfo's decision and it has to make it again regardless.
 *
 * **A rule per path shape is what hier-part already had.**  No change; noted
 * because a reader will look for one name and find four.
 *
 * ## What was missing, and is here now
 *
 * URI-reference, absolute-URI, relative-ref, relative-part, path-noscheme and
 * segment-nz-nc are all in Appendix A and none of them had been pasted -- the
 * first draft of this header took only what URL::parse needed, which is why
 * URL::parse refused a relative reference: there was no rule for one.  RFC
 * 9110 imports four of the six by name, which is what turned the omission up.
 *
 * ## What the grammar does not decide
 *
 * Not whether the URI means anything.  "imap://-b.example/" parses and no
 * server will answer; scheme-specific rules -- which schemes have an
 * authority, what a default port is, whether userinfo is allowed -- are the
 * scheme's business and RFC 3986 says so.
 *
 * Not normalisation.  RFC 3986 section 6 has a whole algorithm for deciding
 * whether two URIs are equivalent: case folding the scheme and host,
 * normalising percent-encoding, removing dot segments.  None of that is here.
 *
 * Not IDN.  A host is octets; RFC 3987's internationalised form and its
 * punycode encoding are not implemented.
 */
namespace rfc3986 {

inline const char* const URI_GRAMMAR = R"ABNF(
URI             =  scheme ":" hier-part [ "?" query ] [ "#" fragment ]

hier-part       =  ( "//" authority path-abempty )
                /  path-absolute
                /  path-rootless
                /  path-empty

URI-reference   =  URI / relative-ref

absolute-URI    =  scheme ":" hier-part [ "?" query ]

relative-ref    =  relative-part [ "?" query ] [ "#" fragment ]

relative-part   =  ( "//" authority path-abempty )
                /  path-absolute
                /  path-noscheme
                /  path-empty

scheme          =  ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )

authority       =  [ userinfo "@" ] host [ ":" port ]
userinfo        =  *( unreserved / pct-encoded / sub-delims / ":" )
host            =  IP-literal / reg-name   ; jlib: IPv4address removed from
                                           ; the middle -- reg-name already
                                           ; matches every dotted quad, and
                                           ; keeping it here made a name like
                                           ; 1.2.3.4.5 fail
port            =  *DIGIT

IP-literal      =  "[" ( IPv6address / IPvFuture ) "]"
IPvFuture       =  "v" 1*HEXDIG "." 1*( unreserved / sub-delims / ":" )

; jlib: restructured.  See the note in the header -- the RFC's nine counted
; alternatives cannot work under possessive repetition, because "*5( h16 ":" )"
; eats the first colon of the "::" it is supposed to stop in front of and ABNF
; has no way to say "not followed by".
IPv6address     =  ( [ ipv6-head ] "::" [ ipv6-tail ] ) / ipv6-tail
ipv6-head       =  h16 *( ":" h16 )
ipv6-tail       =  ipv6-part *( ":" ipv6-part )
ipv6-part       =  IPv4address / h16   ; IPv4address first: it is only ever
                                       ; the last part, and h16 would take
                                       ; the "1" of "1.2.3.4" and stop

h16             =  1*4HEXDIG

IPv4address     =  dec-octet "." dec-octet "." dec-octet "." dec-octet

; jlib: reversed.  As published the bare DIGIT is first and matches the "2"
; of "255", which then commits and strands the rest.
dec-octet       =  "25" %x30-35         ; 250-255
                /  "2" %x30-34 DIGIT    ; 200-249
                /  "1" 2DIGIT           ; 100-199
                /  %x31-39 DIGIT        ; 10-99
                /  DIGIT                ; 0-9

reg-name        =  *( unreserved / pct-encoded / sub-delims )

path-abempty    =  *( "/" segment )
path-absolute   =  "/" [ segment-nz *( "/" segment ) ]
path-noscheme   =  segment-nz-nc *( "/" segment )
path-rootless   =  segment-nz *( "/" segment )
path-empty      =  ""                   ; jlib: the RFC writes 0<pchar>, which
                                        ; is its notation for "nothing"

segment         =  *pchar
segment-nz      =  1*pchar
segment-nz-nc   =  1*( unreserved / pct-encoded / sub-delims / "@" )
pchar           =  unreserved / pct-encoded / sub-delims / ":" / "@"

query           =  *( pchar / "/" / "?" )
fragment        =  *( pchar / "/" / "?" )

pct-encoded     =  "%" HEXDIG HEXDIG
unreserved      =  ALPHA / DIGIT / "-" / "." / "_" / "~"
sub-delims      =  "!" / "$" / "&" / "'" / "(" / ")"
                /  "*" / "+" / "," / ";" / "="
)ABNF";

}
}
}

#endif // JLIB_UTIL_RFC3986_HH
