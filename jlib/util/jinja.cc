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

namespace jlib {
namespace util {
namespace jinja {

/**
 * The grammar, with the two text rules supplied in combinators.
 *
 * "Everything up to the next tag" cannot be written in ABNF here, because
 * repetition is possessive: `*CHAR "{{"` matches to the end of the input and
 * then has no braces left to match.  until() is that rule, and this is the
 * same shape as RFC 3501's `literal`, which counted() supplies in
 * jlib/net/imap_response.cc for the same reason.
 *
 * check() runs after the definitions, not before -- an undefined rule is only
 * an error once everything that was going to define one has.
 */
const abnf::grammar& grammar()
{
    static const abnf::grammar& g = [] () -> const abnf::grammar& {
        static abnf::grammar h = [] {
            abnf::grammar h = abnf::compile(GRAMMAR);

            // Text runs to the next tag *or to the end of the template*,
            // and that second half is why this is a predicate rather than
            // until(): until() requires its terminator to be present, so a
            // template ending in ordinary text -- which most do -- failed on
            // the last run of characters. The rule reads "up to the next
            // opener" either way; only the end-of-input case differs.
            //
            // Requiring at least one byte is what keeps *node terminating.
            // An alternative that can match empty makes the repetition
            // progress without consuming, and the parse never ends.
            h.define("raw-text", abnf::as("raw-text", abnf::where_pure(
                "text up to the next tag",
                [](std::string_view in, std::size_t& at) {
                    const std::size_t start = at;

                    while(at < in.size()) {
                        // A lone "{" is ordinary text; only "{{", "{%" and
                        // "{#" open a tag. At the very end of the input there
                        // is no next byte to look at, so it is text too.
                        if(in[at] == '{' && at + 1 < in.size() &&
                           (in[at + 1] == '{' || in[at + 1] == '%' ||
                            in[at + 1] == '#'))
                            break;

                        at++;
                    }

                    return at > start;
                })));

            h.define("comment-text",
                     abnf::as("comment-text", abnf::until(abnf::lit("#}"))));

            h.check();

            return h;
        }();

        return h;
    }();

    return g;
}

}
}
}
