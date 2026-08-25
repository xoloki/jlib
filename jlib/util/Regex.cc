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

        Regex::Match::Match() {
            m_info = 0;
            m_text = 0;
            m_size = 0;
        }

        Regex::Match::Match(regmatch_t* info, unsigned int size, char* text) {
            m_info = info;
            m_size = size;
            m_text = text;
        }
        
        Regex::Match::Match(const Regex::Match& m) {
            m_info = 0;
            m_text = 0;
            copy(m);
        }

        Regex::Match::~Match() {
            destroy();
        }

        Regex::Match& Regex::Match::operator=(const Regex::Match& m) {
            copy(m);
            return *this;
        }

        void Regex::Match::destroy() {
            if(m_text != 0)
                delete [] m_text;
            m_text = 0;

            if(m_info != 0)
                delete [] m_info;
            m_info = 0;

        }

        void Regex::Match::copy(const Regex::Match& m) {
            destroy();

            m_size = m.m_size;
            
            if(m_size > 0) {
                m_info = new regmatch_t[m_size];
                for(unsigned int i=0; i<m_size; i++) {
                    m_info[i].rm_so = m.m_info[i].rm_so;
                    m_info[i].rm_eo = m.m_info[i].rm_eo;
                }
                
                unsigned int len = std::string(m.m_text).length()+1;
                m_text = new char[len];
                snprintf(m_text, len, "%s", m.m_text);
            }
            else {
                m_text = 0;
                m_info = 0;
            }

        }

        std::string Regex::Match::operator[](unsigned int i) {
            if(i >= m_size) {
                return "";
            }

            if(m_info[i].rm_so == -1 || m_info[i].rm_eo == -1) {
                return "";
            }
            
            return std::string(m_text + m_info[i].rm_so, m_info[i].rm_eo-m_info[i].rm_so);
        }


        Regex::Match::operator bool() {
            return (size() > 0);
        }


        Regex::Regex(const std::string& pattern, int flags) {
            init(pattern, flags);
        }
        
        Regex::Regex(const Regex& r) {
            copy(r);
        }
                
        Regex::~Regex() {
            regfree(&m_regex);
        }
         
        void Regex::init(const std::string& pattern, int flags) {
            m_pattern = pattern;
            m_flags = flags;
            m_size = (1+std::count(pattern.begin(),pattern.end(),'('));
            int err = regcomp(&m_regex, pattern.c_str(), flags);
            if(err != 0) {
                char buf[BUF_SIZE];
                regerror(err, &m_regex, buf, BUF_SIZE);
                throw exception(buf);
            }
        }
       
        Regex::Match Regex::match(const std::string& p_str) {
            regmatch_t* match = new regmatch_t[m_size];
            unsigned int len = p_str.length()+1;
            char* text = new char[len];
            snprintf(text, len, "%s", p_str.c_str());

            int err = regexec(&m_regex, text, m_size, match, 0);
            Match tmp;
            if(err != 0 && err != REG_NOMATCH) {
                char buf[BUF_SIZE];
                regerror(err, &m_regex, buf, BUF_SIZE);
                throw exception(buf);
            }
            else if(err == REG_NOMATCH) {
                delete [] text;
                delete [] match;
            }
            else {
                tmp = Match(match,m_size,text);
            }
            m_last = tmp;
            return tmp;
        }
        
        Regex::Match Regex::operator()(const std::string& str) {
            return match(str);
        }

        Regex& Regex::operator=(const Regex& r) {
            copy(r);
            return *this;
        }

        std::string Regex::operator[](unsigned int i) {
            return m_last[i];
        }


        void Regex::copy(const Regex& r) {
            init(r.m_pattern, r.m_flags);
            m_last = r.m_last;
        }

    }
}
