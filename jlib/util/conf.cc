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

#include <jlib/util/conf.hh>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace jlib {
    namespace util {
        namespace conf {

/**
 * The grammar.
 *
 * nginx has no specification, so this is written from what its configuration
 * files actually look like rather than pasted from anything -- which is the
 * opposite of `rfc9110.hh` and worth saying out loud. Where a real nginx file
 * would be rejected, that is a bug here.
 *
 * Two departures from what a first draft wants, both forced by ordered choice:
 *
 * - **`arg` is tried quoted-first.** A bare token cannot contain a quote, so
 *   trying bare first would match nothing at `"` and then fail the whole
 *   directive rather than backing into the quoted rule.
 *
 * - **`bare` excludes `;`, `{`, `}` and `#` rather than listing what it
 *   allows.** A path, a regular expression and a MIME type are all arguments,
 *   so anything that is not punctuation this grammar owns has to be legal --
 *   listing the allowed characters is how a parser ends up rejecting a
 *   perfectly good `root /srv/www-2.0;` two years later.
 *
 * And one rule that exists only to be permissive:
 *
 * - **`high` is every byte above ASCII, and is legal in a token, inside
 *   quotes, and in a comment.** A POSIX path is a byte string, not text, so a
 *   `root` pointing at a directory with a non-ASCII name has to parse; so does
 *   a comment somebody wrote in their own language. The bytes are passed
 *   through untouched and never decoded -- unlike util::xml::parse(), which
 *   validates UTF-8 first because XML defines its content as characters.
 *   Nothing here defines anything as characters, so there is nothing to
 *   validate and an invalid sequence is somebody else's problem.
 */
static const char* const CONF_GRAMMAR = R"ABNF(
config    = *element
element   = gap / directive
gap       = 1*( SP / HTAB / CR / LF ) / comment
comment   = "#" *( %x20-7E / HTAB / high ) [ CR ] [ LF ]
directive = name *( 1*gap arg ) *gap ( ";" / block )
block     = "{" config "}"
name      = bare
arg       = quoted / bare
bare      = 1*barechar
barechar  = %x21 / %x24-3A / %x3C-7A / %x7C / %x7E / high
quoted    = DQUOTE *qchar DQUOTE
qchar     = escaped / %x20-21 / %x23-5B / %x5D-7E / high
escaped   = "\" ( DQUOTE / "\" / "n" / "t" )
high      = %x80-FF
DQUOTE    = %x22
SP        = %x20
HTAB      = %x09
CR        = %x0D
LF        = %x0A
)ABNF";

const abnf::grammar& grammar() {
    // On first use, not at namespace scope: see util::http::grammar() for why
    // a grammar built while the library loads is a mistake.
    static const abnf::grammar g = [] {
        abnf::grammar built = abnf::compile(CONF_GRAMMAR);

        built.check();

        return built;
    }();

    return g;
}

namespace {

    /** Where `off` is, as a 1-based line and column. */
    void place(const std::string& text, std::size_t off,
               std::size_t& line, std::size_t& column) {
        line = column = 1;

        for(std::size_t i = 0; i < off && i < text.size(); i++) {
            if(text[i] == '\n') { line++; column = 1; }
            else                { column++; }
        }
    }

    /**
     * What could have continued, phrased for somebody editing a config file.
     *
     * Built from the parts rather than from `what()`, which already carries
     * abnf's own prefix and would name two libraries for one failure -- see
     * util::xml::parse(), which had the same choice and made it the same way.
     *
     * **Only the quoted literals survive the filter.** The expected list at a
     * failure holds every character class that could have continued there --
     * `%x24-3A`, `barechar`, `SP`, twenty-odd entries -- which is accurate and
     * of no use whatever to somebody with a config file open.  What helps is
     * the short list of punctuation the grammar owns: ";", "{", "}".
     *
     * `"#"` is dropped with them.  A comment may begin anywhere, so it is in
     * *every* list, and "expected #" is the one answer that is never the
     * problem.
     */
    std::string wanted(const abnf::error& e, const std::string& rest) {
        // **The rule stack answers this better than the expected list does.**
        // Still being inside `quoted` where the parse gave up *means* the
        // closing quote never arrived -- there is no other way to be there at
        // the furthest failure.  Worth taking first, because the expected list
        // for that case leads with the escape backslash and reads as gibberish
        // to somebody whose actual mistake was forgetting a `"`.
        for(const std::string& s : e.rule_stack()) {
            if(s == "quoted") return "unterminated quoted argument";
        }

        std::string out;
        int         n = 0;

        for(const std::string& s : e.expected()) {
            if(s.size() < 3 || s[0] != '"' || s == "\"#\"") continue;
            if(out.find(s) != std::string::npos)              continue;

            out += (n++ ? " or " : "") + s;

            if(n == 3) break;
        }

        if(!out.empty()) return "expected " + out;

        // Nothing actionable survived the filter.  Two cases are common enough
        // to name outright, and both are invisible to the expected list for
        // the same reason: the brace is wrong *because* of what is not open,
        // so the grammar never offered it as a possibility.
        if(!rest.empty() && rest[0] == '}') return "unmatched \"}\"";
        if(!rest.empty() && rest[0] == '{') return "a block needs a name before \"{\"";

        return "unexpected input";
    }

    /** A quoted argument with its escapes resolved; a bare one unchanged. */
    std::string unquoted(const std::string& raw) {
        if(raw.size() < 2 || raw[0] != '"') return raw;

        std::string out;

        for(std::size_t i = 1; i + 1 < raw.size(); i++) {
            if(raw[i] != '\\') { out += raw[i]; continue; }

            if(i + 2 > raw.size() - 1) break;

            switch(raw[++i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            default:  out += raw[i]; break;
            }
        }

        return out;
    }

    /**
     * Matches named `name` inside `m`, **not crossing into a nested block or
     * directive**.
     *
     * `match::all()` is every descendant *at any depth*, which is the right
     * default for pulling a "local-part" out of an address but exactly wrong
     * here: a grammar whose whole point is nesting needs depth-1 answers.
     * Using all() flattened every block into its parent and gave `server`
     * directives the arguments of the directives inside them -- it parsed
     * cleanly and produced a wrong tree, which is the failure mode worth
     * naming at the site.
     *
     * Not `children()` either.  A rule that is part of a larger expression --
     * `arg` inside `*( 1*gap arg )` -- is not necessarily a *child* of the
     * directive, so this descends through structural nodes and stops only at
     * the two that begin a new scope.
     */
    void collect(const abnf::match& m, const char* name, abnf::match::list& out) {
        for(const abnf::match& c : m.children()) {
            const std::string n = c.name();

            if(n == name)                            out.push_back(c);
            else if(n != "block" && n != "directive") collect(c, name, out);
        }
    }

    abnf::match::list within(const abnf::match& m, const char* name) {
        abnf::match::list out;

        collect(m, name, out);

        return out;
    }

    std::vector<directive> walk(const abnf::match& m, const std::string& text);

    directive one(const abnf::match& m, const std::string& text) {
        directive d;
        std::size_t column = 0;

        place(text, m.begin(), d.line, column);

        const abnf::match::list names = within(m, "name");

        if(!names.empty()) d.name = names[0].str();

        for(const abnf::match& a : within(m, "arg")) d.args.push_back(unquoted(a.str()));

        const abnf::match::list blocks = within(m, "block");

        if(!blocks.empty()) {
            d.blocked = true;
            d.block   = walk(blocks[0], text);
        }

        return d;
    }

    std::vector<directive> walk(const abnf::match& m, const std::string& text) {
        std::vector<directive> out;

        for(const abnf::match& d : within(m, "directive")) out.push_back(one(d, text));

        return out;
    }

}

error::error(const std::string& why, std::size_t line, std::size_t column)
    : std::runtime_error(line > 0
                         ? "config line " + std::to_string(line) + ": " + why
                         : "config: " + why),
      m_line(line),
      m_column(column) {}

std::vector<directive> parse(const std::string& text) {
    const abnf::parse_result r = grammar().at("config").try_parse(text);

    // No separate check that the whole file was consumed: try_parse() succeeds
    // only on a complete match, so leftover input arrives here as a failure
    // with the position of the leftovers.  Worth stating, because `config` is
    // `*element` and a repetition cannot fail -- which makes it look like a
    // short parse would slip through silently.  It does not.
    if(!r) {
        const abnf::error& e = r.why();

        throw error(wanted(e, text.substr(std::min(e.offset(), text.size()))),
                    e.line(), e.column());
    }

    return walk(r.root(), text);
}

std::vector<directive> read(const std::string& path) {
    std::ifstream in(path.c_str());

    if(!in) throw error("cannot read \"" + path + "\"");

    std::ostringstream all;

    all << in.rdbuf();

    return parse(all.str());
}

        }
    }
}
