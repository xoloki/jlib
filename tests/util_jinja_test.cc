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

#include <jlib/util/jinja.hh>

#include <iostream>
#include <string>

using jlib::util::abnf::grammar;
using jlib::util::abnf::parse_result;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Does the grammar accept the whole of this template? */
static bool parses(const std::string& tmpl) {
    return static_cast<bool>(
        jlib::util::jinja::grammar().at("template").try_parse(tmpl));
}

static void why(const std::string& tmpl, std::string& out) {
    const parse_result r =
        jlib::util::jinja::grammar().at("template").try_parse(tmpl);

    out = r ? std::string() : r.why().what();
}

static void accepts(const std::string& what, const std::string& tmpl) {
    std::string reason;

    why(tmpl, reason);
    ok(what, reason.empty(), reason);
}

/**
 * The grammar compiles, and every rule it names is defined.
 *
 * raw-text and comment-text are supplied in combinators rather than in ABNF,
 * so a check() that passes is also the assertion that jinja.cc supplied them.
 */
static void the_grammar_is_whole() {
    std::cout << "\nthe grammar is whole:\n";

    bool built = true;
    std::size_t rules = 0;

    try { rules = jlib::util::jinja::grammar().rules().size(); }
    catch(std::exception& e) { built = false; ok("compiles", false, e.what()); }

    if(built) ok("compiles and check()s clean", true);

    ok("has the rules a template needs", rules > 30,
       std::to_string(rules) + " rules");

    ok("nothing left undefined",
       jlib::util::jinja::grammar().undefined().empty());
}

/**
 * The families of chat template that exist in the wild.
 *
 * These are the point of the grammar.  The scanner this replaces looked for
 * <|role|> and could read only the first of them; the other five it either
 * rejected outright or could not see at all.
 */
static void it_reads_the_template_families() {
    std::cout << "\nit reads the template families:\n";

    accepts("Zephyr, which is TinyLlama's",
            "{% for message in messages %}\n"
            "{% if message['role'] == 'user' %}\n"
            "{{ '<|user|>\n' + message['content'] + eos_token }}\n"
            "{% endif %}\n"
            "{% if loop.last and add_generation_prompt %}\n"
            "{{ '<|assistant|>' }}\n"
            "{% endif %}\n"
            "{% endfor %}");

    // The role names sit outside the markers here, which is exactly why the
    // scanner rejected it: it read "im_start" as the role and found no "user".
    accepts("ChatML, as Qwen and Yi use it",
            "{% for message in messages %}{{ '<|im_start|>' + message['role']"
            " + '\n' + message['content'] + '<|im_end|>' + '\n' }}{% endfor %}"
            "{% if add_generation_prompt %}{{ '<|im_start|>assistant\n' }}"
            "{% endif %}");

    accepts("Llama 3, with set and a filter",
            "{% set loop_messages = messages %}{% for message in loop_messages %}"
            "{% set content = '<|start_header_id|>' + message['role'] +"
            " '<|end_header_id|>\n\n' + message['content'] | trim + '<|eot_id|>' %}"
            "{{ content }}{% endfor %}");

    // No pipes at all in the delimiters -- the scanner saw no markers here.
    accepts("Gemma, whose turns are not pipe-delimited",
            "{% for message in messages %}{% if message['role'] == 'user' %}"
            "{{ '<start_of_turn>user\n' + message['content'] | trim +"
            " '<end_of_turn>\n' }}{% endif %}{% endfor %}");

    accepts("Llama 2 and Mistral, which mark turns with [INST]",
            "{% for message in messages %}{% if message['role'] == 'user' %}"
            "{{ '[INST] ' + message['content'] + ' [/INST]' }}{% endif %}"
            "{% endfor %}");
}

/** The pieces those families are built from, one at a time. */
static void it_reads_the_constructs() {
    std::cout << "\nit reads the constructs:\n";

    accepts("bare text with no tags at all", "just text");
    accepts("a lone output", "{{ x }}");
    accepts("an if with elif and else",
            "{% if a %}1{% elif b %}2{% else %}3{% endif %}");
    accepts("a for with its own else", "{% for x in y %}a{% else %}b{% endfor %}");
    accepts("for over two names", "{% for k, v in items %}{{ k }}{% endfor %}");
    accepts("set", "{% set x = 'a' + b %}");
    accepts("comments", "{# nothing to see #}text");
    accepts("whitespace control on both sides",
            "{#- c -#}{%- if x -%}\n  {{- y -}}\n{%- endif -%}");
    accepts("and, or, not, in, and comparison",
            "{% if loop.last and not m.hidden %}{{ 1 }}"
            "{% elif 'x' in m.role or m.n >= 3 %}{{ 2 }}{% endif %}");
    accepts("not in", "{% if 'x' not in y %}a{% endif %}");
    accepts("attribute and subscript together", "{{ messages[0]['content'].strip }}");
    accepts("a call with arguments",
            "{{ raise_exception('first message must be system') }}");
    accepts("a filter with arguments", "{{ x | join(', ') }}");
    accepts("nested blocks",
            "{% for a in b %}{% if c %}{% for d in e %}{{ d }}"
            "{% endfor %}{% endif %}{% endfor %}");
    accepts("a brace that is not a tag", "a { b {c} d");
    accepts("an empty template", "");
    accepts("newlines inside a tag", "{% if a\n    and b %}x{% endif %}");
}

/**
 * What it refuses.
 *
 * A template using a construct outside the subset must fail to parse rather
 * than parse into something that renders almost right.  A chat template that
 * is almost right produces a prompt the model was not tuned on, and no error.
 */
static void it_refuses_what_it_does_not_implement() {
    std::cout << "\nit refuses what it does not implement:\n";

    ok("an unclosed if", !parses("{% if a %}x"));
    ok("an unclosed for", !parses("{% for a in b %}x"));
    ok("endif with no if", !parses("x{% endif %}"));
    ok("a macro", !parses("{% macro m() %}x{% endmacro %}"));
    ok("include", !parses("{% include 'other.j2' %}"));
    ok("an unterminated output", !parses("{{ x "));
    ok("an unterminated comment", !parses("{# x"));
    ok("an empty output", !parses("{{ }}"));
}


/** Is there a node of this name anywhere in the tree? */
static bool contains(const jlib::util::abnf::match& m, const std::string& name) {
    if(m.name() == name) return true;

    const jlib::util::abnf::match::list kids = m.children();

    for(std::size_t i = 0; i < kids.size(); i++)
        if(contains(kids[i], name)) return true;

    return false;
}

static void shaped(const std::string& what, const std::string& tmpl,
                   const std::string& node) {
    const parse_result r =
        jlib::util::jinja::grammar().at("template").try_parse(tmpl);

    ok(what, r && contains(r.root(), node),
       r ? "no " + node + " in the tree" : r.why().what());
}

/**
 * A tag is parsed as a tag, not swallowed as text.
 *
 * This section exists because its absence cost an afternoon.  An earlier
 * raw-text took one byte unconditionally and then ran to the next opener, so
 * when output failed -- on a construct the grammar got wrong -- raw-text
 * consumed the "{{" and the whole tag as literal characters.  Every template
 * still "parsed", and a test that asked only whether it parsed said so.
 *
 * Asking whether it parsed is not the same as asking whether it parsed into
 * the right thing, and only the second question would have caught it.
 */
static void it_parses_tags_as_tags() {
    std::cout << "\nit parses tags as tags:\n";

    shaped("an output is an output, not text", "{{ x }}", "output");
    shaped("a for is a block", "{% for a in b %}x{% endfor %}", "for-block");
    shaped("an if is a block", "{% if a %}x{% endif %}", "if-block");
    shaped("a set is a statement", "{% set x = 'a' %}", "set-stmt");
    shaped("a comment is a comment", "{# c #}", "comment");

    // The construct that hid behind the bug: a quoted newline inside an
    // output.  Read as text, this looks like a parse; read as a tree, the
    // output node is either there or it is not.
    shaped("an output whose string holds a newline",
           "{{ '<|user|>\n' + x }}", "output");

    // And the converse: text that merely looks like a tag stays text.
    shaped("a lone brace stays text", "a { b", "raw-text");
}

int main() {
    the_grammar_is_whole();
    it_reads_the_template_families();
    it_reads_the_constructs();
    it_parses_tags_as_tags();
    it_refuses_what_it_does_not_implement();

    std::cout << "\n" << failures << " failure(s)\n";

    // What a green run does not establish.
    //
    // Not that any of these render correctly -- nothing here evaluates
    // anything.  it_parses_tags_as_tags() checks the shape of the tree, which
    // is a stronger question than "did it parse" and still a weaker one than
    // "does it mean the right thing".  This is a parser test: it says the grammar accepts the shapes
    // and rejects what it does not implement, and says nothing at all about
    // what a template means.  The evaluator, and the assertion that jlib::ai
    // produces byte-identical prompts to the scanner it replaces, are separate.
    //
    // Not that the accepted templates are the real ones.  Five of the six
    // families here are written from the published shape rather than read out
    // of a model file, because only TinyLlama's is on this machine.  A real
    // Qwen or Gemma template may use a construct its family's shape does not
    // show, and this would not know.
    //
    // Not the whole of Jinja.  The subset is deliberate and listed in
    // jinja.hh; the refusals above are a sample of what is outside it and not
    // a proof that everything outside it is refused.
    return failures ? 1 : 0;
}
