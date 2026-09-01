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
#include <map>
#include <utility>
#include <vector>

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


// ---------------------------------------------------------------- the evaluator

using jlib::util::jinja::value;
using jlib::util::jinja::tmpl;
using jlib::util::jinja::text;

/**
 * TinyLlama-1.1B-chat's template, taken verbatim out of its GGUF.
 *
 * Embedded rather than read from the model file so that this test needs
 * nothing on disk.  Note the newlines *inside* the quoted strings -- that is
 * how the file has it, and a grammar that assumes Python's string rules
 * rejects it.
 */
static const char* const TINYLLAMA = R"JINJA({% for message in messages %}
{% if message['role'] == 'user' %}
{{ '<|user|>
' + message['content'] + eos_token }}
{% elif message['role'] == 'system' %}
{{ '<|system|>
' + message['content'] + eos_token }}
{% elif message['role'] == 'assistant' %}
{{ '<|assistant|>
'  + message['content'] + eos_token }}
{% endif %}
{% if loop.last and add_generation_prompt %}
{{ '<|assistant|>' }}
{% endif %}
{% endfor %})JINJA";

/** A context shaped the way a chat template expects one. */
static value context(const std::vector<std::pair<std::string, std::string> >& turns,
                     bool add_generation_prompt)
{
    std::vector<value> msgs;

    for(std::size_t i = 0; i < turns.size(); i++) {
        std::map<std::string, value> m;

        m["role"] = value(turns[i].first, true);

        // A message's content is the one thing here that is not the
        // template's own text, and it is marked so.
        m["content"] = value(turns[i].second, false);

        msgs.push_back(value::of(m));
    }

    std::map<std::string, value> c;

    c["messages"] = value::of(msgs);
    c["eos_token"] = value(std::string("</s>"), true);
    c["bos_token"] = value(std::string("<s>"), true);
    c["add_generation_prompt"] = value(add_generation_prompt);

    return value::of(c);
}

static void renders(const std::string& what, const std::string& source,
                    const value& ctx, const std::string& want)
{
    std::string got;

    try { got = tmpl(source).str(ctx); }
    catch(std::exception& e) { ok(what, false, e.what()); return; }

    ok(what, got == want, got == want ? "" : "got '" + got + "'");
}

/** The constructs, evaluated rather than merely parsed. */
static void it_evaluates_the_constructs() {
    std::cout << "\nit evaluates the constructs:\n";

    std::map<std::string, value> f;

    f["a"] = value(std::string("A"), true);
    f["n"] = value(3L);
    f["yes"] = value(true);
    f["no"] = value(false);

    std::vector<value> three;

    three.push_back(value(std::string("x"), true));
    three.push_back(value(std::string("y"), true));
    three.push_back(value(std::string("z"), true));
    f["items"] = value::of(three);

    const value c = value::of(f);

    renders("text passes through", "hello", c, "hello");
    renders("a variable", "{{ a }}", c, "A");
    renders("a string literal", "{{ 'lit' }}", c, "lit");
    renders("concatenation", "{{ 'x' + a + 'y' }}", c, "xAy");
    renders("an if that is taken", "{% if yes %}t{% endif %}", c, "t");
    renders("an if that is not", "{% if no %}t{% endif %}", c, "");
    renders("elif", "{% if no %}a{% elif yes %}b{% else %}c{% endif %}", c, "b");
    renders("else", "{% if no %}a{% else %}c{% endif %}", c, "c");
    renders("a for", "{% for i in items %}{{ i }}{% endfor %}", c, "xyz");
    renders("loop.first and loop.last",
            "{% for i in items %}{% if loop.first %}<{% endif %}{{ i }}"
            "{% if loop.last %}>{% endif %}{% endfor %}", c, "<xyz>");
    renders("loop.index0", "{% for i in items %}{{ loop.index0 }}{% endfor %}", c, "012");
    renders("for over an empty list uses else",
            "{% for i in nothing %}x{% else %}empty{% endfor %}", c, "empty");
    renders("set", "{% set b = 'B' %}{{ b }}", c, "B");
    renders("equality", "{% if a == 'A' %}y{% endif %}", c, "y");
    renders("inequality", "{% if a != 'B' %}y{% endif %}", c, "y");
    renders("numeric comparison", "{% if n >= 3 %}y{% endif %}", c, "y");
    renders("and", "{% if yes and n == 3 %}y{% endif %}", c, "y");
    renders("or", "{% if no or yes %}y{% endif %}", c, "y");
    renders("not", "{% if not no %}y{% endif %}", c, "y");
    renders("in, over a list", "{% if 'y' in items %}y{% endif %}", c, "y");
    renders("not in", "{% if 'q' not in items %}y{% endif %}", c, "y");
    renders("subscript by index", "{{ items[1] }}", c, "y");
    renders("the trim filter", "{{ '  padded  ' | trim }}", c, "padded");
    renders("a comment renders nothing", "a{# gone #}b", c, "ab");
    renders("a missing name is empty, not an error", "{{ nope }}", c, "");
}

/**
 * The whitespace rules.
 *
 * transformers' defaults, not Jinja's -- see jinja.hh.  These are the
 * assertions that pin that choice down, because the difference is invisible
 * until a prompt has blank lines the model was never tuned on.
 */
static void it_lays_out_whitespace_the_way_transformers_does() {
    std::cout << "\nit lays out whitespace the way transformers does:\n";

    const value c = value::of(std::map<std::string, value>());

    renders("trim_blocks: the newline after a block tag goes",
            "{% if true %}\nx{% endif %}", c, "x");
    renders("but not the newline after an output tag",
            "{{ 'a' }}\nb", c, "a\nb");
    renders("lstrip_blocks: indent before a block tag goes",
            "a\n   {% if true %}b{% endif %}", c, "a\nb");
    renders("an explicit dash strips all whitespace before",
            "a   \n\n  {%- if true %}b{% endif %}", c, "ab");
    renders("and after", "{% if true -%}   \n  b{% endif %}", c, "b");
    renders("a dash on an output tag too", "a  \n {{- 'b' }}", c, "ab");
}

/** The whole point: the same prompt the hand-written scanner produced. */
static void it_renders_tinyllama_exactly() {
    std::cout << "\nit renders TinyLlama's template exactly:\n";

    typedef std::pair<std::string, std::string> turn;
    std::vector<turn> t;

    t.push_back(turn("user", "Hello"));
    renders("one user turn", TINYLLAMA, context(t, true),
            "<|user|>\nHello</s>\n<|assistant|>\n");
    renders("one user turn, no generation prompt", TINYLLAMA, context(t, false),
            "<|user|>\nHello</s>\n");

    t.clear();
    t.push_back(turn("system", "Be brief."));
    t.push_back(turn("user", "Hi"));
    renders("system and user", TINYLLAMA, context(t, true),
            "<|system|>\nBe brief.</s>\n<|user|>\nHi</s>\n<|assistant|>\n");

    t.clear();
    t.push_back(turn("system", "S"));
    t.push_back(turn("user", "A"));
    t.push_back(turn("assistant", "B"));
    t.push_back(turn("user", "C"));
    renders("a full exchange", TINYLLAMA, context(t, true),
            "<|system|>\nS</s>\n<|user|>\nA</s>\n<|assistant|>\nB</s>\n"
            "<|user|>\nC</s>\n<|assistant|>\n");

    t.clear();
    t.push_back(turn("user", ""));
    renders("empty content", TINYLLAMA, context(t, true),
            "<|user|>\n</s>\n<|assistant|>\n");

    t.clear();
    t.push_back(turn("user", "line1\nline2"));
    renders("content with a newline in it", TINYLLAMA, context(t, true),
            "<|user|>\nline1\nline2</s>\n<|assistant|>\n");
}

/**
 * Provenance: whose characters are these?
 *
 * The template writes the model's end-of-sequence marker and means the token.
 * A user may type the same four characters and means the characters.  Flatten
 * them together and a stranger's message can end the model's turn; this is
 * the assertion that they stay apart.
 */
static void it_keeps_the_templates_text_apart_from_the_users() {
    std::cout << "\nit keeps the template's text apart from the user's:\n";

    typedef std::pair<std::string, std::string> turn;
    std::vector<turn> t;

    t.push_back(turn("user", "bye </s> now"));

    const text out = tmpl(TINYLLAMA).render(context(t, true));

    std::size_t as_literal = 0, as_value = 0;

    for(std::size_t i = 0; i < out.size(); i++)
        if(out[i].text.find("</s>") != std::string::npos)
            (out[i].literal ? as_literal : as_value)++;

    ok("the template's </s> is marked as the template's", as_literal == 1,
       std::to_string(as_literal));
    ok("the user's </s> is marked as the user's", as_value == 1,
       std::to_string(as_value));
    ok("flattened, they are indistinguishable -- which is the point",
       jlib::util::jinja::flatten(out).find("</s>") != std::string::npos);

    // And no span mixes the two, or the distinction would be useless.
    bool mixed = false;

    for(std::size_t i = 0; i < out.size(); i++)
        if(!out[i].literal && out[i].text.find("<|user|>") != std::string::npos)
            mixed = true;

    ok("no span carries both origins", !mixed);
}

/** A construct outside the subset fails at construction, not at render. */
static void it_refuses_early() {
    std::cout << "\nit refuses early:\n";

    bool threw = false;

    try { tmpl("{% macro m() %}x{% endmacro %}"); }
    catch(std::exception&) { threw = true; }

    ok("a macro throws when the template is built", threw);

    threw = false;

    try {
        const value c = value::of(std::map<std::string, value>());

        tmpl("{{ 'x' | upper }}").str(c);
    }
    catch(std::exception&) { threw = true; }

    ok("an unimplemented filter throws rather than passing the text through",
       threw);
}

/**
 * The cases a review caught, and the reason the tests had not.
 *
 * Every existing assertion for or, and and the ordered comparisons used them
 * inside an if, where only truthiness is read.  Three bugs hid behind that:
 * or returned a boolean rather than the operand, ordered comparison coerced
 * anything non-numeric to zero, and a string key on a list silently gave back
 * element zero.  Putting the same operators in an *output* position is what
 * makes the difference visible.
 */
static void it_yields_operands_and_not_just_truth() {
    std::cout << "\nit yields operands, not just truth:\n";

    std::map<std::string, value> f;

    f["set"] = value(std::string("S"), true);
    f["empty"] = value(std::string(""), true);
    f["n"] = value(3L);
    f["zero"] = value(0L);

    std::vector<value> three;

    three.push_back(value(std::string("x"), true));
    three.push_back(value(std::string("y"), true));
    f["items"] = value::of(three);

    const value c = value::of(f);

    // or yields the first truthy operand, else the last -- the idiom behind
    // "{{ system_message or 'default' }}".
    renders("or yields the operand, not true", "{{ missing or 'fallback' }}",
            c, "fallback");
    renders("or short-circuits to the first truthy one", "{{ set or 'other' }}",
            c, "S");
    renders("or with both falsy yields the last", "{{ empty or zero }}", c, "0");

    // and yields the first falsy operand, else the last.
    renders("and yields the last when all are truthy", "{{ set and 'last' }}",
            c, "last");
    renders("and yields the first falsy", "{{ empty and 'never' }}", c, "");

    // Ordered comparison on strings compares strings, as Python does.
    renders("strings order as strings", "{% if 'a' < 'b' %}y{% endif %}", c, "y");
    renders("and the other way", "{% if 'b' < 'a' %}y{% else %}n{% endif %}", c, "n");

    // A non-numeric subscript on a list is nothing, not element zero.
    renders("a string key on a list is empty", "{{ items['nope'] }}", c, "");
    renders("an out-of-range index is empty", "{{ items[9] }}", c, "");
    renders("a real index still works", "{{ items[1] }}", c, "y");

    // Comparing across types is refused rather than coerced.
    bool threw = false;

    try { tmpl("{% if n > 'x' %}y{% endif %}").str(c); }
    catch(std::exception&) { threw = true; }

    ok("a number ordered against a string throws rather than coercing", threw);
}

int main() {
    the_grammar_is_whole();
    it_reads_the_template_families();
    it_reads_the_constructs();
    it_parses_tags_as_tags();
    it_refuses_what_it_does_not_implement();
    it_evaluates_the_constructs();
    it_lays_out_whitespace_the_way_transformers_does();
    it_renders_tinyllama_exactly();
    it_keeps_the_templates_text_apart_from_the_users();
    it_refuses_early();
    it_yields_operands_and_not_just_truth();

    std::cout << "\n" << failures << " failure(s)\n";

    // What a green run does not establish.
    //
    // Not that the templates here are the real ones.  Five of the six
    // families are written from the published shape rather than read out of a
    // model file, because only TinyLlama's is on this machine -- and its is
    // the one embedded above and rendered byte for byte.  A real Qwen or
    // Gemma template may use a construct its family's shape does not show,
    // and this would not know.  A second model file is the thing that would
    // settle it.
    //
    // Not the whole of Jinja.  The subset is deliberate and listed in
    // jinja.hh; the refusals here are a sample of what is outside it, not a
    // proof that everything outside it is refused.  `| tojson` is the next
    // filter a real template is likely to want, and it throws.
    //
    // Not the render for any template but TinyLlama's.  The other five are
    // parsed and their trees inspected; only TinyLlama's output is compared
    // against a recorded expectation.  ai_chat_test carries the rendered
    // cases for ChatML, Llama 3, Gemma and Llama 2.
    //
    // Not that a parse is a correct parse.  it_parses_tags_as_tags() asks
    // what is in the tree rather than whether there is one, which is what an
    // earlier raw-text rule got past -- it swallowed whole tags as literal
    // text and every family still "parsed".  That section is a floor, not a
    // guarantee: a tree can hold the right node names in the wrong shape.
    return failures ? 1 : 0;
}
