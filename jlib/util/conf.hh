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

#ifndef JLIB_UTIL_CONF_HH
#define JLIB_UTIL_CONF_HH

#include <jlib/util/abnf.hh>

#include <stdexcept>
#include <string>
#include <vector>

namespace jlib {
    namespace util {

/**
 * A configuration file in the shape nginx uses.
 *
 * ## Why this shape
 *
 * Because jhttpd's flags ran out of room. `--vhost NAME:ROOT:CERT:KEY` packs
 * four fields into colons, `--protect PREFIX:REALM:FILE` packs three, and the
 * two split in opposite directions because a path may contain a colon and a
 * DNS name may not. A format that needs a paragraph to explain which end it
 * parses from is a format asking to be replaced.
 *
 * And because jhttpd#239 wants to read real nginx configuration eventually.
 * Writing that grammar now means the later work is *implementing directives*
 * rather than writing a parser -- which is the expensive half and the one that
 * is easy to get subtly wrong.
 *
 * ## Why a grammar rather than a reader
 *
 * The house norm, and for the reason `util::http` gives: a hand-rolled reader
 * over `find()` and `substr()` accumulates special cases at every site that
 * uses it, and none of them are written down. The grammar is written down,
 * compiled once, and `check()`ed -- and where it departs from what it models,
 * the departure is a comment beside the rule.
 *
 * ## What it is not
 *
 * **This parses; it does not interpret.** No directive means anything here --
 * there is no list of known names, no arity checking, no types. A caller walks
 * the tree and decides. That is deliberate: a parser that knows nginx's several
 * hundred directives is a parser that has to be edited to accept a new one, and
 * the thing most likely to be wrong about a config file is the meaning rather
 * than the punctuation.
 *
 * ## The consequence a caller has to know about
 *
 * **A missing semicolon is not a syntax error here.** A newline is whitespace,
 * so `listen 8080` followed by `root /srv;` on the next line is *one*
 * directive named `listen` with three arguments. That is nginx's own
 * behaviour -- its tokenizer reads to the next `;` or `{` without regard to
 * lines -- and it has to be, or the continuation idiom breaks:
 *
 *     log_format  main  "$remote_addr - $remote_user "
 *                       "\"$request\" $status";
 *
 * So what catches a forgotten `;` is the *arity check* one layer up, when
 * `listen` turns out to be holding three arguments. A caller that does not
 * check how many arguments it got will silently accept a typo.
 */
namespace conf {

class error : public std::runtime_error {
public:
    /**
     * Line and column both count from 1; 0 means "not known", which is what
     * a failure that is not about a position -- an unreadable file -- has.
     */
    error(const std::string& why, std::size_t line = 0, std::size_t column = 0);

    std::size_t line() const { return m_line; }
    std::size_t column() const { return m_column; }

protected:
    std::size_t m_line;
    std::size_t m_column;
};
/**
 * One directive: a name, its arguments, and a block if it had one.
 *
 *     listen 8080;                  name "listen", one argument, no block
 *     server { root /a; }           name "server", no arguments, one inside
 *
 * A directive has a block or a semicolon, never both and never neither -- which
 * is the grammar's business rather than a caller's.
 */
struct directive {
    std::string              name;
    std::vector<std::string> args;
    std::vector<directive>   block;

    /** True if this was written with braces, even if they were empty. */
    bool blocked = false;

    /** 1-based, for an error a person can act on. */
    std::size_t line = 0;

    /** The argument at `i`, or "" -- so a caller need not count first. */
    const std::string& arg(std::size_t i) const {
        static const std::string none;

        return i < args.size() ? args[i] : none;
    }
};

/** The grammar, compiled once.  Exposed so a test can ask what it knows. */
const abnf::grammar& grammar();

/**
 * Parse `text`.
 *
 * @throws error with a line number
 */
std::vector<directive> parse(const std::string& text);

/**
 * Read and parse a file.
 *
 * @throws error naming the file if it cannot be read
 */
std::vector<directive> read(const std::string& path);

}

    }
}

#endif
