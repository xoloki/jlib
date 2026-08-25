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
 * The parts of RFC 5322 that are not about addresses.
 *
 * Section 3.2's lexical tokens, which the mail RFCs share, and section 3.3's
 * date-time, which is read by jlib::util::Date.
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

/**
 * Section 3.3, the Date: header.  Appended to LEXICAL.
 *
 * Section 4.3's obsolete forms are all here and all *first*, because every one
 * of them subsumes the modern form it sits beside -- obs-day is
 * "[CFWS] 1*2DIGIT [CFWS]" where day requires a trailing FWS, obs-year takes
 * two digits where year takes four -- so under ordered choice the modern form
 * would match a prefix, commit, and strand the rest.  The same reordering, for
 * the same reason, as jlib/net/rfc5322.hh's OBSOLETE block.
 *
 * There is no policy switch here.  A Date: header is not something a client
 * gets to be strict about: it is written by whatever sent the message, it is
 * frequently obsolete syntax, and refusing to read one means refusing to sort
 * a mailbox.
 */
inline const char* const DATE_TIME = R"ABNF(
; 3.3
date-time       =  [ day-of-week "," ] date time [CFWS]

day-of-week     =  obs-day-of-week / ([FWS] day-name)
day-name        =  "Mon" / "Tue" / "Wed" / "Thu" / "Fri" / "Sat" / "Sun"

date            =  day month year
day             =  obs-day / ([FWS] day-digits FWS)
month           =  "Jan" / "Feb" / "Mar" / "Apr" / "May" / "Jun" /
                   "Jul" / "Aug" / "Sep" / "Oct" / "Nov" / "Dec"
year            =  obs-year / (FWS year-digits FWS)

time            =  time-of-day zone
time-of-day     =  hour ":" minute [ ":" second ]
hour            =  obs-hour / hour-digits
minute          =  obs-minute / minute-digits
second          =  obs-second / second-digits

; jlib: names for the digits, which the RFC writes inline.  The obsolete
; forms wrap each field in [CFWS], so the span of "day" can be " 1 (the 2nd) "
; -- and a comment is allowed to contain digits.  Scraping them out of the
; span would read that comment as part of the date.
day-digits      =  1*2DIGIT
year-digits     =  2*DIGIT
hour-digits     =  2DIGIT
minute-digits   =  2DIGIT
second-digits   =  2DIGIT

; jlib: [CFWS] before the numeric form, which the RFC writes as FWS.  A
; Date: header that folds before its zone, or carries a comment there, is
; common and means what it says.
zone            =  ([CFWS] zone-sign zone-offset) / ([CFWS] obs-zone)
zone-sign       =  "+" / "-"
zone-offset     =  4DIGIT

; 4.3
obs-day-of-week =  [CFWS] day-name [CFWS]
obs-day         =  [CFWS] day-digits [CFWS]
obs-year        =  [CFWS] year-digits [CFWS]
obs-hour        =  [CFWS] hour-digits [CFWS]
obs-minute      =  [CFWS] minute-digits [CFWS]
obs-second      =  [CFWS] second-digits [CFWS]

; jlib: "UTC" added, and first, because it is not RFC 5322 and it is
; everywhere -- without it "UT" matches and strands the "C".  The single
; letters come last for the same reason: "G" would take the first character
; of "GMT".
obs-zone        =  "UTC" / "UT" / "GMT" /
                   "EST" / "EDT" / "CST" / "CDT" /
                   "MST" / "MDT" / "PST" / "PDT" /
                   %d65-73 / %d75-90 / %d97-105 / %d107-122
)ABNF";

}
}
}

#endif // JLIB_UTIL_RFC5322_HH
