/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2001 Joey Yandle <xoloki@gmail.com>
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

#include <jlib/sys/sys.hh>

#include <jlib/util/util.hh>
#include <jlib/util/Headers.hh>
#include <jlib/util/content_type.hh>
#include <jlib/util/encoded_word.hh>

#include <utility>

#include <sstream>
#include <algorithm>

namespace jlib {
    namespace util {
        Headers::Headers() 
            : m_length(0)
        {
            
        }

        Headers::Headers(const std::string& s) 
            : m_length(0)
        {
            parse(s);
        }

        
        std::string Headers::operator[](const std::string& key) const {
            // On a local; this overwrote its own parameter.
            const std::string k = upper(key);
            return get(k);
        }

        Headers::operator std::string() const {
            std::ostringstream o;

            std::map<std::string,u_int> count;
            
            for(list_type::const_iterator i = m_keys.begin();i!=m_keys.end();i++) {
                if(count.find(*i) == count.end()) {
                    count[*i]=0;
                }
                u_int n = count[*i];
                
                const_iterator k = find(*i);
                if(k != end()) {
                    list_type::const_iterator l = k->second.begin();
                    
                    for(u_int j=0;j<n;j++) if(l != k->second.end()) l++;
                    
                    if(l != k->second.end())
                        o << studly_caps(*i) << ": " << *l << std::endl;

                    count[*i]++;
                }
            }

            return o.str();
        }
        
        std::string Headers::get(const std::string& key) const {
            std::string charset;
            return get(key, charset);
        }

        std::string Headers::get(const std::string& key, std::string& charset) const {
            // On a local; this overwrote its own parameter.
            const std::string k = upper(key);
            map_type::const_iterator i = m_charset_map.find(k);
            if(i != m_charset_map.end()) {
                charset = i->second.front();
            }
            
            if(empty(key)) {
                return "";
            }
            else {
                return *begin(key);
            }
        }

        void Headers::set(const std::string& key, const std::string& val) {
            // On a local; this overwrote its own parameter.
            const std::string k = upper(key);

            list_type& l = m_map[k];
            l.clear();
            l.push_back(val);

            list_type::iterator i = std::find(m_keys.begin(),m_keys.end(),k);
            if(i == m_keys.end()) {
                m_keys.push_back(k);
            }
            else {
                i++;
                std::remove(i,m_keys.end(),key);
            }
        }

        void Headers::add(const std::string& key, const std::string& val) {
            // On a local; this overwrote its own parameter.
            const std::string k = upper(key);
            m_map[k].push_back(val);
            m_keys.push_back(k);
        }

        
        void Headers::append(const std::string& key, const std::string& val) {
            // On a local; this overwrote its own parameter.
            const std::string k = upper(key);
            list_type& list = m_map[k];
            if(list.size() == 0) {
                add(k,val);
            }
            else {
                list.front() += val;
            }
        }

        
        Headers::list_type Headers::keys() const {
            return m_keys;
        }

        Headers::list_type Headers::vals(const std::string& key) const {
            // On a local; this overwrote its own parameter.
            const std::string k = upper(key);
            
            const_iterator i = find(k);
            if(i != end()) {
                return i->second;
            }
            else {
                return Headers::list_type();
            }
        }

        
        void Headers::parse(const std::string& s, bool uppercase) {
            // Moved into the stream rather than copied.  This took the whole
            // message by value and then copied it again into the istringstream,
            // so parsing a 2M message cost 4M before it read a byte.  Taking by
            // value and moving costs one copy for a variable and none for a
            // temporary, and does not change the signature.
            std::istringstream stream(std::move(s));
            if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                std::cerr <<"enter jlib::util::Headers::parse()"<<std::endl;
            }
            clear();

            std::string buf, key, val, charset;
            map_type::iterator current = end();
            list_type::iterator kcurrent;
            list_type::iterator lcurrent;

            map_type::iterator char_current;
            list_type::iterator char_list_current;

            u_int current_length = 0;

            while(!stream.eof()) {

                jlib::sys::getline(stream,buf);
                if(static_cast<std::string::size_type>(stream.tellg()) != s.npos)
                    current_length = stream.tellg();
                if(buf == "") {
                    break;
                }
                else {
                    if(isspace(buf[0]) && current != end()) {
                        val += (" "+trim(buf));
                        if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                            //std::cerr <<"\tfolded header, now " << key << " => " << val <<std::endl;
                            std::cerr <<"\tfolded header" << std::endl;
                        }
                    }
                    else {
                        if(current != end()) {
                            val = decode(val, charset);
                            *lcurrent = val;
                            *char_list_current = charset;

                            if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                                std::cerr <<"\tupdated " << key << " => " << val << ": charset " << charset<<std::endl;
                            }
                        }

                        current = end();
                        std::string::size_type j,k;
                        if( (j=buf.find(":")) != buf.npos && 
                            (k=buf.find_last_of("\t ",j)) == buf.npos ) {

                            key = jlib::util::upper(buf.substr(0,j));
                            val = jlib::util::trim(buf.substr(j+1));
                            charset = "";

                            current = m_map.insert(end(),std::make_pair(key,list_type()));
                            kcurrent = m_keys.insert(m_keys.end(),key);
                            lcurrent = current->second.insert(current->second.end(),val);

                            char_current = m_charset_map.insert(m_charset_map.end(),std::make_pair(key,list_type()));
                            char_list_current = char_current->second.insert(char_current->second.end(),charset);

                            if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                                std::cerr <<"\tinserted " << key << " => " << val <<std::endl;
                            }
                        }
                    }
                }
            }

            if(current != end()) {
                val = decode(val, charset);
                *lcurrent = val;
            }

            m_length = current_length;

            if(find("content-type") != end()) {
                // The fourth hand-rolled copy of MIME parameter parsing in the
                // tree, and it had every bug the other three had: tokenize on
                // ";" splits inside a quoted value, so boundary="a;b" was cut
                // in half; find("charset=") anywhere rather than a whole-name
                // comparison, so a boundary containing those letters won; and
                // the value kept its quotes, so m_charset came back as
                // "utf-8" with the quotation marks still on it and never
                // matched a charset name anywhere else.
                //
                // content_type is in util now precisely so this could go --
                // it used to be in jlib/net, which util cannot depend on.
                try {
                    m_charset = content_type::parse(get("content-type")).get("charset");
                }
                catch(content_type::exception&) {
                    // A Content-Type that will not parse has no charset in it
                    // to find.  RFC 2045 5.2 says to treat an unreadable one
                    // as text/plain, which carries us-ascii by default, and
                    // "" is what this reported for that case before.
                    m_charset.clear();
                }
            }

            if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                std::cerr <<"leave jlib::util::Headers::parse()"<<std::endl;
            }
        }
                /*
                  if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                    std::cerr <<"\tcalling jlib::sys::getline"<<std::endl;
                }
                jlib::sys::getline(stream,buf);
                if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                    std::cerr <<"\tafter getline(), buf=\""
                              <<buf<<"\""<<std::endl
                              <<"\tstream.tellg()="<<stream.tellg()
                              <<std::endl;
                }
                if(buf == "") {
                    if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                        std::cerr <<"\tfound blank line, "
                                  << "setting bunny = false"<<std::endl;
                    }
                    bunny = false;
                }
                else {
                    if(isspace(buf[0])) {
                        if(current != end()) {
                            val += jlib::util::trim(buf);
                            if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                                std::cerr <<"\tfound whitespace, val=\""
                                          <<val<<"\""<<std::endl;
                            }
                            current->second = val;
                        }
                    }
                    else {
                        if(current != end()) {
                            std::string buffer = current->second;
                            if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                                std::cerr <<"\tdecoding, current->second=\""
                                          <<current->second<<"\""<<std::endl;
                            }
                            if(buffer.find("=") != std::string::npos) {
                                if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                                    std::cerr <<"\tdecoding, found '='"<<std::endl;
                                }
                                buffer = current->second;
                                jlib::util::Regex reg("(.*)=\\?(.+)\\?([QqBb])\\?(.+)\\?=(.*)");
                                while(reg(buffer)) {
                                    std::string enc = reg[4];
                                    std::string dec;
                                    if(jlib::util::upper(reg[3]) == "B") {
                                        dec = jlib::crypt::base64::decode(enc);
                                    }
                                    else if(jlib::util::upper(reg[3]) == "Q") {
                                        dec = jlib::crypt::qp::decode(enc);
                                    }

                                    current->second = reg[1]+dec+reg[5];
                                    buffer = current->second;
                                }
                                if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                                    std::cerr <<"\tafter decoding, current->second=\""
                                              <<current->second<<"\""<<std::endl;
                                }

                            }
                        }

                        key = "";
                        val = "";
                        current = end();
                        std::string::size_type j;
                        if( (j=buf.find(":")) != buf.npos ) {
                            key = buf.substr(0,j);
                            if(uppercase)
                                key = jlib::util::upper(key);
                            val = jlib::util::trim(buf.substr(j+1));
                            if(key.find("FROM ") == 0) {
                                key = "";
                                val = "";
                            }
                            else {
                            current = m_map.insert(std::make_pair(key,val));
                            }
                            }
                        }
                    }
            }
            if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                std::cerr <<"jlib::util::Headers::parse(): leaving"<<std::endl;
            }

            m_length = stream.tellg();

            //return count;

                */

        /**
         * By value deliberately: val is the working buffer, decoded in place
         * and returned.
         */
        std::string Headers::decode(const std::string& val, std::string& charset) {
            // This used to be forty lines of find() and substr(), with the two
            // bug classes this repository has spent the year on.
            //
            // "m = val.find(\"?\", z)" was never checked against npos, so a
            // value with an unbalanced "=?" in it read val[1] and took the
            // whole rest of the string as the charset.  And after
            // "val.replace(i, k + 2 - i, dec)" it resumed from k -- an index
            // into the string as it had been *before* the replacement, which
            // is shorter now -- so a header with two encoded words could lose
            // the second.  The same shape as the util::recode loop.
            std::vector<std::string> charsets;
            const std::string out = rfc2047::decode(val, charsets);

            charset = charsets.empty() ? std::string() : charsets.front();

            return out;
        }

        std::string Headers::encode(const std::string& val, const std::string& charset) {
            return rfc2047::encode(val, charset);
        }

        u_int Headers::get_length() const {
            return m_length;
        }

            Headers::iterator Headers::find(const std::string& key) { return m_map.find(upper(key)); }
            Headers::const_iterator Headers::find(const std::string& key) const { return m_map.find(upper(key)); }

            Headers::iterator Headers::begin() { return m_map.begin(); }
            Headers::const_iterator Headers::begin() const { return m_map.begin(); }
            Headers::iterator Headers::end() { return m_map.end(); }
            Headers::const_iterator Headers::end() const { return m_map.end(); }
            Headers::reverse_iterator Headers::rbegin() { return m_map.rbegin(); }
            Headers::const_reverse_iterator Headers::rbegin() const { return m_map.rbegin(); }
            Headers::reverse_iterator Headers::rend() { return m_map.rend(); }
            Headers::const_reverse_iterator Headers::rend() const { return m_map.rend(); }
            bool Headers::empty() const { return m_map.empty(); }
            Headers::size_type Headers::size() const { return m_map.size(); }

            void Headers::clear() { m_map.clear(); }

            Headers::list_type::iterator Headers::begin(const std::string& key) { return m_map.find(upper(key))->second.begin(); }
            Headers::list_type::const_iterator Headers::begin(const std::string& key) const { return m_map.find(upper(key))->second.begin(); }
            Headers::list_type::iterator Headers::end(const std::string& key) { return m_map.find(upper(key))->second.end(); }
            Headers::list_type::const_iterator Headers::end(const std::string& key) const { return m_map.find(upper(key))->second.end(); }
            Headers::list_type::reverse_iterator Headers::rbegin(const std::string& key) { return m_map.find(upper(key))->second.rbegin(); }
            Headers::list_type::const_reverse_iterator Headers::rbegin(const std::string& key) const { return m_map.find(upper(key))->second.rbegin(); }
            Headers::list_type::reverse_iterator Headers::rend(const std::string& key) { return m_map.find(upper(key))->second.rend(); }
            Headers::list_type::const_reverse_iterator Headers::rend(const std::string& key) const { return m_map.find(upper(key))->second.rend(); }
            bool Headers::empty(const std::string& key) const { return (m_map.find(upper(key)) == m_map.end() ||
                                                        m_map.find(upper(key))->second.empty()); }
            Headers::size_type Headers::size(const std::string& key) const { return m_map.find(upper(key))->second.size(); }

            void Headers::clear(const std::string& key) { if(!empty(upper(key))) m_map.find(upper(key))->second.clear(); }

            std::string Headers::get_charset() { 
                return m_charset;
            }



        
    }
}
