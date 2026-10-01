/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_UTIL_REGEX_HH
#define JLIB_UTIL_REGEX_HH

#include <iostream>
#include <exception>
#include <string>
#include <vector>
#include <regex.h>

namespace jlib {
    namespace util {

        /**
         * POSIX regular expressions, as a value.
         *
         * A thin wrapper over <regex.h>: regcomp on construction, regexec on
         * match(), regfree on destruction, with the results in a type that
         * owns its own storage.  ERE by default; pass REG_ICASE and friends
         * through the flags.
         *
         * ## jlib does not use this, and that is not an accident
         *
         * Both callers it ever had were matching things that have a
         * published grammar -- a dotted quad and a mail address -- and both
         * were wrong about them.  The IPv4 one accepted 999.999.999.999 and
         * found addresses inside timestamps.  Where a format has an RFC,
         * util::abnf and the rfc*.hh grammars read it as written and this
         * will not.  Reach for a regex when there is no grammar to reach
         * for.
         *
         * ## Two things to know
         *
         * **operator[] reads the last match**, so a Regex is stateful and
         * `r(s)` then `r[1]` is the documented idiom.  That also makes
         * sharing one Regex across threads a data race even though nothing
         * looks mutable -- hold the Match that match() returns instead, and
         * that problem goes away.
         *
         * **Copying recompiles.**  A regex_t owns heap the POSIX API gives
         * no way to duplicate, so a copy pays regcomp again; nothing says
         * regexec on one regex_t from two threads is safe either.
         */
        class Regex {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& p_msg = "") {
                    m_msg = "jlib::util::Regex::exception: "+p_msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            /**
             * One result: where each group landed, and the subject it
             * landed in.
             *
             * **A std::string and a std::vector, not two owning raw
             * pointers.**  The hand-written copy constructor, assignment and
             * destructor this replaces had the whole family of bugs those
             * invite -- `m = m` freed both arrays and then read out of them,
             * which segfaults.  With members that own themselves the special
             * members are correct by default, and moving works too.
             */
            class Match {
            public:
                Match() = default;

                /**
                 * Group `i`, or "" if there is no such group or it did not
                 * participate -- POSIX reports the latter as rm_so == -1.
                 */
                std::string operator[](unsigned int i) const;

                /** Groups plus one, since group 0 is the whole match. */
                unsigned int size() const {
                    return static_cast<unsigned int>(m_info.size());
                }

                /**
                 * Whether anything matched.
                 *
                 * explicit, so `if(m)` and `!m` still work but `int n = m;`
                 * no longer quietly compiles.
                 */
                explicit operator bool() const { return !m_info.empty(); }

            private:
                Match(std::vector<regmatch_t> info, std::string text)
                    : m_info(std::move(info)), m_text(std::move(text)) {}

                std::vector<regmatch_t> m_info;
                std::string m_text;

                // Only a Regex can build a meaningful one.  This used to be
                // a public constructor taking two raw pointers that it then
                // owned, which a caller had no way to use correctly.
                friend class Regex;
            };

            Regex(const std::string& pattern="", int flags=REG_EXTENDED);
            Regex(const Regex& r);
            ~Regex();

            Match match(const std::string& str);
            Match operator()(const std::string& str);

            Regex& operator=(const Regex& r);

            /** Group `i` of the most recent match; see match(). */
            std::string operator[](unsigned int i) const;

        protected:
            void init(const std::string& pattern, int flags);

            regex_t m_regex;
            std::string m_pattern;
            int m_flags;

            /**
             * The last result, which operator[] reads.
             *
             * It makes Regex stateful, and the call sites were written
             * against that -- `if(r(s))` then `r[1]`.  Prefer holding the
             * Match that match() returns; this is kept because it is the
             * documented interface, not because it is the better one.
             */
            Match m_last;
        };
        
    }
}

#endif //JLIB_UTIL_REGEX_HH
