/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */

#ifndef JLIB_NET_RFC5322_HH
#define JLIB_NET_RFC5322_HH

namespace jlib {
namespace net {

/**
 * RFC 5322's address grammar, as ABNF, for jlib::util::abnf::compile().
 *
 * This is the thing the parsing rewrite exists for: the address syntax is the
 * RFC's own text rather than a transcription into find() and substr(), so what
 * jlib accepts can be checked against the document by reading it.
 *
 * Four pieces.  CORE is what every policy shares; STRICT and OBSOLETE are two
 * spellings of the six productions the RFC gives an obsolete form to, and
 * exactly one of them is appended to CORE; LENIENT is a further "=/" extension
 * for text that is not RFC 5322 at all.  See jlib/net/address.hh.
 *
 * ## Where this departs from the published text, and why
 *
 * Every departure is marked in the text below with a "; jlib:" comment.  There
 * are three kinds and no others.
 *
 * **Names for spans the RFC leaves unnamed.**  A rule name is the only handle a
 * grammar read from text has, so a value that has to be extracted needs a rule
 * around it.  atom-text, qs-body, dtext-body and phrase-dot exist for that, and
 * dot-atom-text is respelled in terms of atom-text.  Each is a pure
 * refactoring: the language accepted is unchanged.
 *
 * **Reordered alternatives.**  jlib::util::abnf is PEG ordered choice, so the
 * first alternative that matches wins and the rest are never tried -- and it
 * does not re-enter a choice that has already committed.  Where the RFC's order
 * would commit to a shorter match and strand the rest of the input, the
 * alternatives are swapped.  In every case here the obsolete form subsumes the
 * modern one, so putting it first changes nothing about what is accepted, only
 * about what is tried.  The rule of thumb is in the header docs for
 * abnf::compile(): a longer alternative shadowed by a shorter prefix of itself.
 *
 * **Obsolete productions not carried at all.**  obs-qp, obs-ctext, obs-qtext,
 * obs-dtext, obs-utext, obs-body and obs-NO-WS-CTL are absent on purpose.  They
 * permit NUL and bare CR and LF inside an address, which is not legacy syntax
 * so much as a header-injection primitive: a parsed address containing a bare
 * CR goes on to be written into a "MAIL FROM:" command.  obs-FWS is absent for
 * the same reason in miniature -- it exists to allow a fold with no leading
 * whitespace on the continuation line, and nothing needs it.
 *
 * ## What this grammar is not
 *
 * Not RFC 5321.  Syntax is not deliverability: a@-b.example parses here and no
 * mail server will accept it.  There are no length limits and no LDH hostname
 * rules.
 *
 * Not RFC 6532.  atext is ASCII, so an internationalised address is rejected
 * rather than mangled.  Adding it is one line and it is spelled out in
 * tests/net_address_test.cc, because "=/" is exactly what that RFC is written
 * as.
 *
 * Not RFC 2047.  An encoded word in a display name comes back as the literal
 * "=?utf-8?q?...?=" it was written as; decoding it is util::Headers' job.
 */
namespace rfc5322 {

/**
 * Everything the policies share: 3.2 lexical tokens, 3.4.1 addr-spec, and the
 * 3.4 shapes whose obsolete form is elsewhere.
 */
inline const char* const CORE = R"ABNF(
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
quoted-string   =  [CFWS] DQUOTE qs-body [FWS] DQUOTE [CFWS]
qs-body         =  *([FWS] qcontent)          ; jlib: names what is between the
                                              ; quotes

; 3.2.5 miscellaneous tokens
word            =  atom / quoted-string

; 3.4.1 addr-spec specification
addr-spec       =  local-part "@" domain
domain-literal  =  [CFWS] "[" dtext-body [FWS] "]" [CFWS]
dtext-body      =  *([FWS] dtext)             ; jlib: names what is between the
                                              ; brackets
dtext           =  %d33-90 / %d94-126

; 3.4 address specification
address         =  group / mailbox            ; jlib: reordered.  As published
                                              ; this is mailbox / group
mailbox         =  name-addr / addr-spec
name-addr       =  [display-name] angle-addr
display-name    =  phrase
group           =  display-name ":" [group-list] ";" [CFWS]
)ABNF";

/**
 * RFC 5322 as it stands today: section 3, and none of section 4.
 *
 * Every alternative here is in the RFC's own order, because with the obsolete
 * forms gone there is nothing left to shadow anything.
 */
inline const char* const STRICT = R"ABNF(
local-part      =  dot-atom / quoted-string
domain          =  dot-atom / domain-literal
angle-addr      =  [CFWS] "<" addr-spec ">" [CFWS]
phrase          =  1*word
mailbox-list    =  mailbox *("," mailbox)
address-list    =  address *("," address)
group-list      =  mailbox-list / CFWS
)ABNF";

/**
 * Section 4's obsolete syntax, which real mail is full of.
 *
 * Five things, and they are worth naming because between them they are most of
 * what a strict parser trips over in a real mailbox:
 *
 *   obs-phrase       "Joe Q. Public <a@b>" -- a period in a display name
 *   obs-local-part   comments and spaces around the dots of a local part
 *   obs-domain       the same, in the domain
 *   obs-*-list       leading, doubled and trailing commas in a header
 *   obs-route        "<@a.example:joe@b.example>", accepted and discarded
 *
 * The obsolete alternative is written first in each rule, which is the reverse
 * of the RFC.  That is forced: each one subsumes the modern form, so under
 * ordered choice the modern form would match a prefix, win, and strand the
 * rest.  "joe . bloggs@x.com" is the shortest demonstration -- dot-atom matches
 * "joe " including the trailing CFWS, addr-spec then wants "@" and finds ".",
 * and nothing re-enters the choice to try obs-local-part.
 */
inline const char* const OBSOLETE = R"ABNF(
local-part      =  obs-local-part / dot-atom / quoted-string
obs-local-part  =  word *("." word)
domain          =  obs-domain / dot-atom / domain-literal
obs-domain      =  atom *("." atom)
angle-addr      =  ([CFWS] "<" addr-spec ">" [CFWS]) / obs-angle-addr
obs-angle-addr  =  [CFWS] "<" obs-route addr-spec ">" [CFWS]
obs-route       =  obs-domain-list ":"
obs-domain-list =  *(CFWS / ",") "@" domain *("," [CFWS] ["@" domain])
phrase          =  obs-phrase
obs-phrase      =  word *(word / phrase-dot / CFWS)
phrase-dot      =  "."                        ; jlib: names the "." so a
                                              ; display name can be put back
                                              ; together in order
mailbox-list    =  obs-mbox-list
obs-mbox-list   =  *([CFWS] ",") mailbox *("," [mailbox / CFWS])
address-list    =  obs-addr-list
obs-addr-list   =  *([CFWS] ",") address *("," [address / CFWS])
group-list      =  mailbox-list / CFWS / obs-group-list
obs-group-list  =  1*([CFWS] ",") [CFWS]
)ABNF";

/**
 * Not RFC 5322 at all: an angle bracket with nothing to match it.
 *
 * "<joe@x.com" and "joe@x.com>" turn up in real headers, and a mail client
 * that cannot find the address in them is not much use.  Appended with "=/"
 * so these are the last alternatives tried, and only reached once every
 * well-formed reading has failed -- a lenient parse of well-formed input is
 * the same parse as a strict one.
 *
 * They extend name-addr rather than mailbox, which is not arbitrary.  mailbox
 * is "name-addr / addr-spec", and a stray ">" is only stray because addr-spec
 * matched everything before it -- appending to mailbox would put these after
 * the alternative that already won, and ordered choice never gets back to
 * them.  Appending to name-addr puts them ahead of it instead.
 *
 * It is a grammar extension rather than string surgery before the parse for a
 * reason.  The predecessor of this code repaired its input by hand, in a
 * function called salvage(), and the result was two different notions of what
 * an address is disagreeing with each other depending on which entry point a
 * caller happened to use.
 */
inline const char* const LENIENT = R"ABNF(
name-addr       =/ [display-name] [CFWS] "<" addr-spec [CFWS]
name-addr       =/ addr-spec ">" [CFWS]
)ABNF";

}
}
}

#endif // JLIB_NET_RFC5322_HH
