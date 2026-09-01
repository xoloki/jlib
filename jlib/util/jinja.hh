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

#ifndef JLIB_UTIL_JINJA_HH
#define JLIB_UTIL_JINJA_HH

#include <jlib/util/abnf.hh>

#include <exception>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jlib {
namespace util {

/**
 * A Jinja2 subset, as ABNF -- enough to render a chat template.
 *
 * Jinja has no published grammar to paste, unlike the RFC headers beside this
 * one.  It is defined by its implementation, so this is written from the
 * language reference and from the templates that have to work, and a reader
 * cannot check it against a document line by line.  That is the difference
 * worth knowing before trusting it.
 *
 * Read by jlib::util::abnf::compile(); see jlib/util/jinja.cc for the
 * evaluator and jlib/ai/chat.cc for what uses it.
 *
 * ## The subset, and why this one
 *
 * A chat template is a small program that turns a list of messages into a
 * prompt.  In practice it uses: text, `{{ }}`, `{% for %}`, `{% if %}` with
 * `elif` and `else`, `{% set %}`, string and boolean literals, attribute and
 * subscript access, `+` on strings, comparison, `and`/`or`/`not`/`in`, a
 * couple of filters, and `raise_exception()`.  That is what is here.
 *
 * Not here: macros, `{% include %}`, `{% extends %}`, `{% block %}`, tests
 * (`is defined`), arithmetic beyond `+`, and the filter library.  A template
 * using one does not render badly -- it does not compile, and says which
 * construct it was.  A chat template that renders *almost* right produces a
 * prompt the model was not tuned on and no error, which is the failure this
 * refuses to have.
 *
 * ## Where this departs from Jinja
 *
 * Each marked "; jlib:" in the grammar below.
 *
 * **Blocks trim the newline that follows them, and leading whitespace before
 * them is stripped.**  Jinja's defaults are the opposite on both counts, but
 * `transformers` renders chat templates with `trim_blocks=True` and
 * `lstrip_blocks=True`, and a template is written against whatever rendered
 * it.  TinyLlama's puts a newline after every `{% %}`; with Jinja's defaults
 * each one reaches the prompt and the model sees blank lines it was never
 * tuned on.  This is a property of the evaluator rather than the grammar, and
 * it is stated here because it is the departure most likely to be assumed
 * away.
 *
 * **Keywords match case-insensitively.**  ABNF's char-val is case-insensitive
 * and the text front end honours that, so `{% IF %}` compiles here and is a
 * syntax error in Jinja.  Accepting more than the language does is the safe
 * direction, and no template writes it.
 *
 * **raw-text is supplied in combinators.**  "Text up to the next tag" is not
 * expressible in ABNF -- repetition here is possessive, so `*CHAR "{{"` eats
 * the braces -- and until() is what expresses it.  Same shape as RFC 3501's
 * `literal`, which counted() supplies for the same reason.
 *
 * **Whitespace inside a tag may include newlines.**  Written as `ows` rather
 * than the RFCs' WSP, because a template may break a long condition across
 * lines and Jinja does not care.
 */
namespace jinja {

/**
 * The compiled grammar, with raw-text and comment-text supplied in
 * combinators -- see jinja.cc for why they cannot be written in ABNF.
 *
 * Compiled once, on first use.
 */
const abnf::grammar& grammar();


/**
 * A piece of rendered text, and where it came from.
 *
 * `literal` means the characters are the template's own -- its string
 * literals, its punctuation, the newlines in its layout.  False means they
 * came from a value the caller supplied.
 *
 * The distinction is the whole reason render() does not return a string.  A
 * chat template writes the model's end-of-sequence marker itself, and that
 * has to become the *token*; a user who types the same characters into a
 * message has written characters, and they have to stay characters.  Flatten
 * the two together and there is no way to tell them apart afterwards, which
 * is how a stranger's message comes to end the model's turn.
 */
struct span {
    std::string text;
    bool literal;
};

/**
 * Rendered text.
 *
 * A list rather than a string, so that provenance survives concatenation:
 * "a" + content + "b" is three spans, not one.  Adjacent spans of the same
 * origin are not merged, because nothing needs them to be.
 */
typedef std::vector<span> text;

/** The characters, with the provenance discarded. */
std::string flatten(const text& t);

/** A span list holding one literal string. */
text literal_text(std::string s);

/**
 * A value a template can see.
 *
 * Strings are span lists rather than std::string for the reason above: a
 * value carries its provenance, and concatenating two values concatenates
 * theirs.  Everything else a chat template needs -- booleans for
 * add_generation_prompt, integers for loop.index0, lists of messages, maps of
 * role and content -- has no provenance to carry.
 */
class value {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg)
            : m_msg("jlib::util::jinja::value::exception: " + msg) {}
        const char* what() const throw() { return m_msg.c_str(); }
    private:
        std::string m_msg;
    };

    enum class kind { none, boolean, number, string, list, map };

    value();
    explicit value(bool b);
    explicit value(long n);

    /** A string. Literal by default: most strings in flight are the template's. */
    explicit value(std::string s, bool literal = true);
    explicit value(text t);

    static value of(std::vector<value> items);
    static value of(std::map<std::string, value> fields);

    kind type() const { return m_kind; }

    /** Jinja truthiness: none and false are false, "" is false, [] and {} are false, 0 is false. */
    bool truthy() const;

    const text& str() const;
    std::string flat() const;
    long number() const;

    const std::vector<value>& items() const;
    const std::map<std::string, value>& fields() const;

    bool has(const std::string& key) const;
    const value& at(const std::string& key) const;

    /** Equality as the template sees it; strings compare by characters. */
    bool operator==(const value& o) const;
    bool operator!=(const value& o) const { return !(*this == o); }

private:
    kind m_kind;
    bool m_bool;
    long m_number;
    text m_text;
    std::vector<value> m_items;
    std::map<std::string, value> m_fields;
};

/**
 * A parsed template.
 *
 * Parsed once at construction -- a template that cannot be read, or that uses
 * a construct outside the subset, throws there rather than at the first
 * render.  See the note in this file on why that is the behaviour worth
 * having.
 */
class tmpl {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg)
            : m_msg("jlib::util::jinja::tmpl::exception: " + msg) {}
        const char* what() const throw() { return m_msg.c_str(); }
    private:
        std::string m_msg;
    };

    explicit tmpl(std::string source);

    const std::string& source() const { return *m_source; }

    /** Render, keeping provenance. */
    text render(const value& context) const;

    /** Render and flatten. */
    std::string str(const value& context) const;

private:
    // Held by shared_ptr because the match tree's text() views point into it:
    // a copy or a move of a plain std::string member would leave them
    // dangling, and this class is otherwise perfectly copyable.
    std::shared_ptr<const std::string> m_source;
    abnf::match m_root;
};

inline const char* const GRAMMAR = R"ABNF(
template       =  *node

; A node never matches an end tag -- not if-block, which needs "if" first,
; nor raw-text, which stops at "{%".  That is what lets *node inside a block
; stop at its own terminator despite the repetition being possessive.
node           =  comment / if-block / for-block / set-stmt / output / raw-text

comment        =  "{#" comment-text "#}"

output         =  "{{" [ "-" ] ows expr ows [ "-" ] "}}"

; ------------------------------------------------------------------- blocks

if-block       =  if-tag *node *elif-part [ else-part ] endif-tag
if-tag         =  tag-open "if" 1*ws expr tag-close
elif-part      =  elif-tag *node
elif-tag       =  tag-open "elif" 1*ws expr tag-close
else-part      =  else-tag *node
else-tag       =  tag-open "else" tag-close
endif-tag      =  tag-open "endif" tag-close

; The for-else of Jinja: taken when the sequence was empty.
for-block      =  for-tag *node [ else-part ] endfor-tag
for-tag        =  tag-open "for" 1*ws name-list 1*ws "in" 1*ws expr tag-close
endfor-tag     =  tag-open "endfor" tag-close

; "for k, v in items" -- one name or several.
name-list      =  name *( ows "," ows name )

set-stmt       =  tag-open "set" 1*ws name ows "=" ows expr tag-close

tag-open       =  "{%" [ "-" ] ows
tag-close      =  ows [ "-" ] "%}"

; --------------------------------------------------------------- expressions
;
; One rule per precedence level, loosest first, each built from the next.
; Ordered choice makes the usual longest-first care necessary in compare-op,
; where "<" would otherwise win against "<=".

expr           =  or-expr
or-expr        =  and-expr *( 1*ws "or" 1*ws and-expr )
and-expr       =  not-expr *( 1*ws "and" 1*ws not-expr )
not-expr       =  ( "not" 1*ws not-expr ) / comparison
; jlib: "x is defined" is a *test*, not a comparison, and real templates lean
; on it -- Qwen and Llama 3 both branch on whether tools were supplied.  It
; sits at this level because that is where Jinja binds it.
comparison     =  concat [ ( compare-op concat ) / is-test ]
is-test        =  1*ws "is" 1*ws [ "not" 1*ws ] name

; jlib: the word operators carry their own whitespace, so that a name
; beginning "in" -- "index" -- is not read as the operator plus "dex".
compare-op     =  ( ows symbolic-op ows )
               /  ( 1*ws "not" 1*ws "in" 1*ws )
               /  ( 1*ws "in" 1*ws )
symbolic-op    =  "==" / "!=" / ">=" / "<=" / ">" / "<"

; jlib: arithmetic beyond "+" is here because real templates need it --
; Gemma alternates roles with "loop.index0 % 2 == 0".  The two levels are
; Jinja's own precedence: * / // % bind tighter than + and -.  "//" comes
; before "/" because ordered choice takes the first branch that matches.
concat         =  term *( ows add-op ows term )
add-op         =  "+" / "-"
term           =  filtered *( ows mul-op ows filtered )
mul-op         =  "*" / "//" / "/" / "%"
filtered       =  postfix *filter

postfix        =  atom *( attribute / subscript / call-args )
attribute      =  "." name
; jlib: a slice, then a plain index.  Slice first because ordered choice takes
; the first branch that matches and "[1]" fails the slice on its missing colon,
; while "[1:]" would match the index rule's expr and then choke on the colon.
subscript      =  slice / index
slice          =  "[" ows [ expr ] ows ":" ows [ expr ] ows "]"
index          =  "[" ows expr ows "]"
call-args      =  "(" ows [ arg-list ] ows ")"
; jlib: a keyword argument.  Llama 3 writes "tojson(indent=4)", and the indent
; is not cosmetic -- the model was tuned on the indented form, so the argument
; has to be read and honoured rather than accepted and dropped.
arg-list       =  arg *( ows "," ows arg )
arg            =  kwarg / expr
kwarg          =  name ows "=" ows expr

; jlib: a filter is postfix like the others, but "|" binds looser than a call
; -- "a | trim" is trim(a), not a(|trim).  Kept out of postfix for that.
filter         =  ows "|" ows name [ call-args ]

atom           =  string / number / boolean / none / group / name
group          =  "(" ows expr ows ")"

; ------------------------------------------------------------------ literals

string         =  dq-string / sq-string
dq-string      =  DQUOTE *dq-char DQUOTE
sq-string      =  "'" *sq-char "'"
; jlib: a string literal may contain a raw newline.  Python's cannot, and the
; resemblance makes it easy to assume Jinja's cannot either -- but TinyLlama's
; template writes '<|user|>' followed by an actual LF inside the quotes, so a
; grammar without this rejects the very first template it is given.
dq-char        =  escape / HTAB / CR / LF / %x20-21 / %x23-5B / %x5D-7E / %x80-FF
sq-char        =  escape / HTAB / CR / LF / %x20-26 / %x28-5B / %x5D-7E / %x80-FF
escape         =  "\" ( DQUOTE / "'" / "\" / "n" / "t" / "r" )

number         =  1*DIGIT
boolean        =  "true" / "false" / "True" / "False"
none           =  "none" / "None"

name           =  ( ALPHA / "_" ) *( ALPHA / DIGIT / "_" )

; jlib: newlines count as whitespace inside a tag, so a long condition may be
; broken across lines the way a template author would expect.
ows            =  *ws

; jlib: named ws, not wsp -- ABNF rule names are case-insensitive, so a
; rule called wsp would BE core WSP and its definition would recurse.
ws             =  WSP / CR / LF
)ABNF";

}
}
}

#endif // JLIB_UTIL_JINJA_HH
