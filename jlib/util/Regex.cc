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

#include <jlib/util/Regex.hh>

#include <sstream>
#include <algorithm>

#include <cstdio>

const int BUF_SIZE=1024;

namespace jlib {
    namespace util {

        std::string Regex::Match::operator[](unsigned int i) const {
            if(i >= m_info.size())
                return "";

            // A group that did not participate -- the b in "(a)(b)?" against
            // "a" -- is reported as -1, not as an empty range.
            if(m_info[i].rm_so == -1 || m_info[i].rm_eo == -1)
                return "";

            return m_text.substr(std::string::size_type(m_info[i].rm_so),
                                 std::string::size_type(m_info[i].rm_eo - m_info[i].rm_so));
        }

        Regex::Regex(const std::string& pattern, int flags) {
            init(pattern, flags);
        }

        Regex::Regex(const Regex& r) {
            // Recompiled rather than shared: a regex_t owns heap the POSIX
            // API gives no way to duplicate, and nothing says regexec on one
            // from two threads is safe.
            init(r.m_pattern, r.m_flags);

            m_last = r.m_last;
        }

        Regex::~Regex() {
            regfree(&m_regex);
        }

        void Regex::init(const std::string& pattern, int flags) {
            m_pattern = pattern;
            m_flags = flags;

            int err = regcomp(&m_regex, pattern.c_str(), flags);

            if(err != 0) {
                char buf[BUF_SIZE];
                regerror(err, &m_regex, buf, BUF_SIZE);
                throw exception(buf);
            }
        }

        Regex::Match Regex::match(const std::string& p_str) {
            // re_nsub, not a count of '(' in the pattern.  Counting them
            // included escaped parens and parens inside a bracket
            // expression, so "[(]" claimed a group that does not exist;
            // regcomp has already worked out the real number.
            const std::size_t groups = m_regex.re_nsub + 1;

            std::vector<regmatch_t> info(groups);

            int err = regexec(&m_regex, p_str.c_str(), groups, info.data(), 0);

            if(err != 0 && err != REG_NOMATCH) {
                char buf[BUF_SIZE];
                regerror(err, &m_regex, buf, BUF_SIZE);
                throw exception(buf);
            }

            // Built once.  This used to construct a Match owning two raw
            // arrays, deep-copy it into a local, deep-copy that into
            // m_last, and deep-copy that again on return.
            m_last = (err == REG_NOMATCH)
                ? Match()
                : Match(std::move(info), p_str);

            return m_last;
        }

        Regex::Match Regex::operator()(const std::string& str) {
            return match(str);
        }

        Regex& Regex::operator=(const Regex& r) {
            if(this == &r)
                return *this;

            // Compile the new pattern *before* releasing the old one.
            //
            // This used to call copy(), which calls init(), which calls
            // regcomp straight over m_regex -- leaking the compiled regex
            // already there, on every assignment.  Measured at 238 MB over
            // 200k assignments.  Doing it in the obvious other order would
            // trade that for something worse: if the new regcomp failed,
            // init() throws and the destructor then regfrees a regex that
            // was already freed.
            regex_t built;

            int err = regcomp(&built, r.m_pattern.c_str(), r.m_flags);

            if(err != 0) {
                char buf[BUF_SIZE];
                regerror(err, &built, buf, BUF_SIZE);
                throw exception(buf);
            }

            regfree(&m_regex);

            m_regex   = built;
            m_pattern = r.m_pattern;
            m_flags   = r.m_flags;
            m_last    = r.m_last;

            return *this;
        }

        std::string Regex::operator[](unsigned int i) const {
            return m_last[i];
        }

    }
}
