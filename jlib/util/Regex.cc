/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joe Yandle <jwy@divisionbyzero.com>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
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


        Regex::Regex(std::string pattern, int flags) {
            init(pattern, flags);
        }
        
        Regex::Regex(const Regex& r) {
            copy(r);
        }
                
        Regex::~Regex() {
            regfree(&m_regex);
        }
         
        void Regex::init(std::string pattern, int flags) {
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
       
        Regex::Match Regex::match(std::string p_str) {
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
        
        Regex::Match Regex::operator()(std::string str) {
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
