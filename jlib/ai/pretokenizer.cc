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

#include <jlib/ai/pretokenizer.hh>
#include <jlib/ai/unicode.hh>

#include <functional>

namespace jlib {
namespace ai {
namespace pretokenizer {

namespace {

/**
 * One UTF-8 codepoint, or 0 if there is not one here.
 *
 * Strict about continuation bytes: a truncated or malformed sequence is not a
 * codepoint, so a rule asking for a letter will not accidentally accept half
 * of one and leave the parse mid-character.
 *
 * Strict about overlong forms and surrogates too, which matters more than it
 * looks: C1 81 is a structurally valid two-byte sequence decoding to 'A', so
 * without the check it would be one `letters` chunk rather than two bytes
 * that are not a character.  Rejecting here is what sends them to
 * `stray-byte`, where the byte map encodes them and nothing is lost.
 */
std::size_t decode(std::string_view in, std::size_t at, unsigned int& cp)
{
    if(at >= in.size()) return 0;

    const unsigned char c = static_cast<unsigned char>(in[at]);
    std::size_t n = 0;

    if(c < 0x80)         { cp = c;        n = 1; }
    else if(c < 0xC0)    return 0;                    // a stray continuation
    else if(c < 0xE0)    { cp = c & 0x1F; n = 2; }
    else if(c < 0xF0)    { cp = c & 0x0F; n = 3; }
    else if(c < 0xF8)    { cp = c & 0x07; n = 4; }
    else                 return 0;

    if(at + n > in.size()) return 0;

    for(std::size_t i = 1; i < n; i++) {
        const unsigned char k = static_cast<unsigned char>(in[at + i]);

        if((k & 0xC0) != 0x80) return 0;

        cp = (cp << 6) | (k & 0x3F);
    }

    // The shortest form is the only form: a codepoint that would have fitted
    // in fewer bytes was not encoded, it was smuggled.
    static const unsigned int least[] = { 0, 0, 0x80, 0x800, 0x10000 };

    if(cp < least[n]) return 0;

    // Surrogates are not characters, and nothing above the last plane is one.
    if((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return 0;

    return n;
}

/** A rule matching one codepoint that satisfies a predicate. */
util::abnf::rule codepoint(std::string what, std::function<bool(unsigned int)> pred)
{
    return util::abnf::where_pure(
        std::move(what),
        [pred](std::string_view in, std::size_t& at) {
            unsigned int cp = 0;
            const std::size_t n = decode(in, at, cp);

            if(n == 0 || !pred(cp)) return false;

            at += n;

            return true;
        });
}

bool eol(unsigned int cp) { return cp == '\r' || cp == '\n'; }

}

bool supported(const std::string& pre)
{
    // An empty key is the older files, which predate anyone writing it down
    // and are all llama-bpe.  Anything else named is a different pattern and
    // this is not it.
    return pre.empty() || pre == "llama-bpe";
}

const util::abnf::grammar& grammar()
{
    static const util::abnf::grammar g = [] {
        util::abnf::compile_options o;

        // The prose rules are supplied here rather than defined afterwards,
        // which is what compile_options::prose is for: the grammar keeps
        // saying what each class *is*, in the text a reader checks against
        // the pattern, and this says what ABNF cannot.
        o.prose = [](std::string_view what) -> util::abnf::rule {
            const std::string s(what);

            if(s == "one codepoint with general category L")
                return codepoint("a letter", is_letter);

            if(s == "one codepoint with general category N")
                return codepoint("a number", is_number);

            if(s == "one codepoint with the White_Space property")
                return codepoint("whitespace", is_space);

            if(s == "CR or LF")
                return codepoint("CR or LF", eol);

            if(s == "one codepoint that is none of White_Space, L or N")
                return codepoint("neither space, letter nor number",
                    [](unsigned int cp) {
                        return !is_space(cp) && !is_letter(cp) &&
                               !is_number(cp);
                    });

            // \s*[\r\n]+ .  The whitespace run, cut after the last CR
            // or LF inside it -- so "  \n  " is "  \n" and then "  ",
            // and "\n\n\n" is all three.  Structurally this would be
            // "*space-char 1*eol-char", which cannot work: a newline is
            // whitespace, so a possessive repetition of whitespace eats
            // the newlines the second half is waiting for.
            if(s == "whitespace up to and including its last CR or LF")
                return util::abnf::where_pure(
                    "whitespace ending in a newline",
                    [](std::string_view in, std::size_t& at) {
                        std::size_t end = at, after_eol = at;

                        for(;;) {
                            unsigned int cp = 0;
                            const std::size_t n = decode(in, end, cp);

                            if(n == 0 || !is_space(cp)) break;

                            end += n;

                            if(eol(cp)) after_eol = end;
                        }

                        if(after_eol == at) return false;

                        at = after_eol;

                        return true;
                    });

            // A byte that is not the start of a valid sequence.  decode()
            // is what decides, so this accepts exactly what every other rule
            // here rejects, and the two cannot drift apart.
            if(s == "one byte that begins no UTF-8 sequence")
                return util::abnf::where_pure(
                    "a byte that is not part of a character",
                    [](std::string_view in, std::size_t& at) {
                        unsigned int cp = 0;

                        if(at >= in.size() || decode(in, at, cp) != 0)
                            return false;

                        at += 1;

                        return true;
                    });

            if(s == "one codepoint that is none of CR, LF, L or N")
                return codepoint("neither CR, LF, letter nor number",
                    [](unsigned int cp) {
                        return !eol(cp) && !is_letter(cp) && !is_number(cp);
                    });

            // \s+(?!\S).  A run of whitespace hands its last character to
            // whatever follows -- "   x" is "  " then " x" -- unless
            // nothing follows, in which case it keeps the lot.  A run of
            // one followed by anything therefore matches nothing here and
            // falls through to the plain \s+ branch below it, which is
            // what the regex does by backtracking.
            if(s == "whitespace less its last character, unless it ends "
                    "the input")
                return util::abnf::where_pure(
                    "whitespace, less its last character",
                    [](std::string_view in, std::size_t& at) {
                        std::size_t end = at, last = at;

                        for(;;) {
                            unsigned int cp = 0;
                            const std::size_t n = decode(in, end, cp);

                            if(n == 0 || !is_space(cp)) break;

                            last = end;
                            end += n;
                        }

                        if(end == at) return false;

                        const std::size_t stop =
                            end < in.size() ? last : end;

                        if(stop == at) return false;

                        at = stop;

                        return true;
                    });

            throw exception("pretokenizer: no implementation for the "
                                "prose rule <" + s + ">");
        };

    util::abnf::grammar h = util::abnf::compile(LLAMA_BPE, o);

        h.check();

        return h;
    }();

    return g;
}

std::vector<std::string> split(const std::string& text)
{
    std::vector<std::string> out;

    if(text.empty()) return out;

    const util::abnf::parse_result r = grammar().at("chunks").try_parse(text);

    if(!r)
        throw exception(std::string("pretokenizer: this text does not "
                                        "split: ") + r.why().what());

    const util::abnf::match root =
        r.root().name() == "chunks" ? r.root() : r.root()["chunks"];

    const util::abnf::match::list kids = root.children();

    for(std::size_t i = 0; i < kids.size(); i++)
        if(kids[i].name() == "chunk") out.push_back(std::string(kids[i].text()));

    return out;
}

}
}
}
