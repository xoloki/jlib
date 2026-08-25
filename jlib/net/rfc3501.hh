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

#ifndef JLIB_NET_RFC3501_HH
#define JLIB_NET_RFC3501_HH

namespace jlib {
namespace net {

/**
 * IMAP4rev1 responses, from RFC 3501 section 9, as ABNF.
 *
 * The half of section 9 a client reads.  Commands are what jlib writes and it
 * knows what it wrote; responses are what a server sends and they have to be
 * taken as they come.
 *
 * Read by jlib::util::abnf::compile(); see jlib/net/imap_response.hh for what
 * uses it.  Self-contained -- IMAP is not RFC 822 and borrows none of its
 * lexical layer.
 *
 * ## literal is deliberately not defined here
 *
 * RFC 3501 4.3 writes it
 *
 *     literal = "{" number "}" CRLF *CHAR8
 *             ; Number represents the number of CHAR8s
 *
 * with the actual constraint in a *comment*, because ABNF cannot say "this
 * many of them".  This grammar therefore references `literal` and never
 * defines it; imap_response.cc supplies it in combinators with abnf's
 * counted(), and only then calls check().
 *
 * That seam is the reason jlib::util::abnf has two public layers rather than
 * one.  It was argued for in the plan on the strength of this exact
 * production, three branches before anything could use it.
 *
 * ## Where this departs from the published text
 *
 * Marked with "; jlib:", as the other grammars here are.
 *
 * **An attribute value is matched by shape, not by production.**  RFC 3501
 * gives ENVELOPE and BODYSTRUCTURE full grammars -- roughly sixty productions
 * between them -- and jlib does not interpret either.  Transcribing sixty
 * productions in order to ignore what they match would be worse than saying
 * so: att-value accepts a balanced parenthesised group, a string, or a number.
 * The attributes jlib *does* read -- FLAGS, UID, RFC822.SIZE, RFC822 and its
 * variants, BODY[...] -- come back with their values parsed.
 *
 * This also means an attribute nobody here has heard of does not fail the
 * response.  MODSEQ, X-GM-MSGID and every other extension a server volunteers
 * parse as a name and a value, which is what a client should do with them.
 *
 * **Reordered alternatives**, as everywhere: mailbox is astring rather than
 * "INBOX" / astring, because astring subsumes it and ordered choice would
 * otherwise take the "INBOX" of "INBOXES" and strand the rest.  INBOX being
 * case-insensitive is a rule about what a mailbox name *means*, not about how
 * it is written, and belongs in the code that compares them.
 *
 * **TEXT-CHAR runs to %xFF.**  RFC 3501 defines CHAR as %x01-7F, so a server
 * that puts UTF-8 in a human-readable error message is out of spec -- and
 * they do it, and refusing to read the response is a worse answer than
 * reading a message jlib will not interpret anyway.
 *
 * ## What is not here
 *
 * Commands, which jlib writes.  ENVELOPE, BODYSTRUCTURE and the STATUS
 * attributes as productions, per the note above.  Anything from an extension:
 * this is RFC 3501, not 4466, 4551, 6154 or 9051.
 */
namespace rfc3501 {

inline const char* const RESPONSE = R"ABNF(
; 9.  One response: a continuation, an untagged data line, or the tagged
; completion of a command.
response        =  continue-req / response-data / response-tagged

continue-req    =  "+" [ SP resp-text ] CRLF
response-tagged =  rtag SP resp-cond-state CRLF
response-data   =  "*" SP ( resp-cond-state / mailbox-data / message-data /
                            capability-data ) CRLF

; jlib: named rtag.  "tag" is what abnf calls the thing a capture is named
; with, and having a rule of that name in a grammar it reads is asking for a
; misreading by whoever comes next.
rtag            =  1*ASTRING-CHAR

; jlib: one rule for what the RFC splits across resp-cond-state,
; resp-cond-auth and resp-cond-bye -- they differ only in which of the five
; names they allow, and which are legal where is a rule about the state the
; connection is in rather than about the text.
resp-cond-state =  cond-name SP resp-text
cond-name       =  "OK" / "NO" / "BAD" / "PREAUTH" / "BYE"

resp-text       =  [ "[" resp-text-code "]" SP ] text
resp-text-code  =  1*( %x01-5C / %x5E-FF )   ; jlib: "any TEXT-CHAR except ]"
text            =  *TEXT-CHAR

; 7.2
mailbox-data    =  ( "FLAGS" SP flag-list )
                /  ( list-name SP mailbox-list )
                /  ( "SEARCH" *( SP number ) )
                /  ( "STATUS" SP mailbox SP att-group )
                /  ( number SP count-name )
list-name       =  "LIST" / "LSUB"
count-name      =  "EXISTS" / "RECENT"

mailbox-list    =  flag-list SP ( delim / nil ) SP mailbox
delim           =  DQUOTE QUOTED-CHAR DQUOTE
mailbox         =  astring        ; jlib: "INBOX" / astring, reordered away

capability-data =  "CAPABILITY" *( SP capability )
capability      =  1*ATOM-CHAR

; 7.4
message-data    =  number SP ( "EXPUNGE" / ( "FETCH" SP msg-att ) )
msg-att         =  "(" [ msg-att-item *( SP msg-att-item ) ] ")"

; jlib: FLAGS first and by name, because it is one of the five jlib reads and
; its value has a shape worth keeping.  Everything else is a name and a value.
msg-att-item    =  ( "FLAGS" SP flag-list )
                /  ( att-name [ att-section ] SP att-value )
; jlib: "-" and "_" added.  Every attribute RFC 3501 itself defines is
; [A-Z0-9.], but an extension's is not -- X-GM-MSGID is the one everybody
; meets -- and an unknown attribute should parse as a name and a value rather
; than fail the whole response.
att-name        =  1*( ALPHA / DIGIT / "." / "-" / "_" )
; jlib: [ "." number ] added.  A response carries the origin octet alone --
; "BODY[]<0>" -- and the "<origin.length>" form belongs to the command; a
; server that echoes the command's form back would otherwise desynchronise the
; connection rather than merely say something odd.
att-section     =  "[" *( %x01-5C / %x5E-FF ) "]" [ "<" number [ "." number ] ">" ]
att-value       =  att-group / nstring / number
att-group       =  "(" *( att-group / nstring / number / att-atom / SP ) ")"
att-atom        =  1*ATOM-CHAR

; 9, flags
flag-list       =  "(" [ flag *( SP flag ) ] ")"
flag            =  ( "\" ( "*" / atom ) ) / atom
atom            =  1*ATOM-CHAR

; 4.3, strings.  literal is referenced and never defined: see the header note.
astring         =  string / 1*ASTRING-CHAR
string          =  quoted / literal
quoted          =  DQUOTE quoted-body DQUOTE
quoted-body     =  *QUOTED-CHAR       ; jlib: names what is between the quotes
nstring         =  string / nil
nil             =  "NIL"
number          =  1*DIGIT

; 9, character classes.  The RFC states these as exclusions in prose; these
; are those exclusions worked out as ranges, and the test spells out every
; excluded character.
;
;   atom-specials   = "(" / ")" / "{" / SP / CTL / list-wildcards /
;                     quoted-specials / resp-specials
;   list-wildcards  = "%" / "*"
;   quoted-specials = DQUOTE / "\"
;   resp-specials   = "]"
ATOM-CHAR       =  %x21 / %x23-24 / %x26-27 / %x2B-5B / %x5E-7A / %x7C-7E
ASTRING-CHAR    =  ATOM-CHAR / "]"
TEXT-CHAR       =  %x01-09 / %x0B-0C / %x0E-FF   ; jlib: past %x7F, see above
QUOTED-CHAR     =  %x01-09 / %x0B-0C / %x0E-21 / %x23-5B / %x5D-FF
                /  ( "\" ( DQUOTE / "\" ) )
)ABNF";

}
}
}

#endif // JLIB_NET_RFC3501_HH
