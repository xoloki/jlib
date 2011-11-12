/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 2001 Joe Yandle <jwy@divisionbyzero.com>
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

#include <jlib/crypt/crypt.hh>

#include <jlib/sys/sys.hh>

#include <jlib/util/util.hh>
#include <jlib/util/Headers.hh>

#include <sstream>
#include <algorithm>

namespace jlib {
    namespace util {
        Headers::Headers() 
            : m_length(0)
        {
            
        }

        Headers::Headers(std::string s) 
            : m_length(0)
        {
            parse(s);
        }

        Headers::~Headers() {
            
        }

        
        std::string Headers::operator[](std::string key) const {
            key = upper(key);
            return get(key);
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
        
        std::string Headers::get(std::string key) const {
            std::string charset;
            return get(key, charset);
        }

        std::string Headers::get(std::string key, std::string& charset) const {
            key = upper(key);
            map_type::const_iterator i = m_charset_map.find(key);
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

        void Headers::set(std::string key, std::string val) {
            key = upper(key);

            list_type& l = m_map[key];
            l.clear();
            l.push_back(val);

            list_type::iterator i = std::find(m_keys.begin(),m_keys.end(),key);
            if(i == m_keys.end()) {
                m_keys.push_back(key);
            }
            else {
                i++;
                std::remove(i,m_keys.end(),key);
            }
        }

        void Headers::add(std::string key, std::string val) {
            key = upper(key);
            m_map[key].push_back(val);
            m_keys.push_back(key);
        }

        
        void Headers::append(std::string key, std::string val) {
            key = upper(key);
            list_type& list = m_map[key];
            if(list.size() == 0) {
                add(key,val);
            }
            else {
                list.front() += val;
            }
        }

        
        Headers::list_type Headers::keys() const {
            return m_keys;
        }

        Headers::list_type Headers::vals(std::string key) const {
            key = upper(key);
            
            const_iterator i = find(key);
            if(i != end()) {
                return i->second;
            }
            else {
                return Headers::list_type();
            }
        }

        
        void Headers::parse(std::string s, bool uppercase) {
            std::istringstream stream(s);
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
                std::string cs = "charset=";
                std::string ctype = get("content-type");
                std::vector<std::string> ctypev = tokenize(ctype, ";");
                for(std::vector<std::string>::iterator x = ctypev.begin(); x != ctypev.end(); x++) {
                    if(x->find(cs) != std::string::npos) {
                        m_charset = trim(x->substr(x->find(cs)+cs.length()));
                        break;
                    }
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

        std::string Headers::decode(std::string val, std::string& charset) {
            //static jlib::util::Regex reg("(.*)=\\?(.+)\\?([QqBb])\\?(.+)\\?=(.*)");
            static const std::string BEGIN = "=?", END = "?=";
            if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                std::cerr <<"enter jlib::util::Headers::decode()"<<std::endl;
            }
            //jlib::util::Regex::Match m;
            if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                std::cerr <<"\tparsing for RFC1522 header"<<std::endl;
            }
            
            std::string::size_type i,j=0,k,m,n, z;
            std::string enc, dec;

            while( (i=val.find(BEGIN,j)) != val.npos ) {
            //while( (m=reg(val)) ) {
                if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                    std::cerr <<"\tfound BEGIN"<<std::endl;
                }

                z = i+BEGIN.length();

                if( (k=val.find(END,z)) != val.npos ) {
                    if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                        std::cerr <<"\tfound END"<<std::endl;
                        std::cerr <<"\t" << val.substr(z, k-z) <<std::endl;
                    }
                    m = val.find("?", z);
                    if(val[m+2] == '?') {
                        charset = val.substr(z, m-z);
                        if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                            std::cerr <<"\tfound charset: "<< charset << std::endl;
                        }
                        enc = val.substr(m+3, k-m-3);
                        dec = enc;

                        if(std::toupper(val[m+1]) == 'B') {
                            dec = jlib::crypt::base64::decode(enc);
                        }
                        else if(std::toupper(val[m+1]) == 'Q') {
                            dec = jlib::crypt::qp::decode(enc);
                        }
                        
                        if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                            std::cerr <<"\tdecoded "<<enc << " into " << dec << std::endl;
                        }
                        val.replace(i, k+END.length()-i, dec);
                    }
                }
                
                j = k;
            }

            

            if(getenv("JLIB_UTIL_HEADERS_DEBUG")) {
                std::cerr <<"leave jlib::util::Headers::decode()"<<std::endl;
            }
            return val;
        }

        std::string Headers::encode(std::string val, std::string charset) {
            bool high = false;
            std::string::size_type i = 0, p, x;
            std::string ret;

            while((p = find_high(val, i)) != std::string::npos) {
                ret += val.substr(i, (p-i));
                x = val.find(" ", p);
                if(x != std::string::npos) {
                    ret += ("=?" + charset + "?B?" + crypt::base64::encode(val.substr(p, (x-p))) + "?=");
                } else {
                    ret += ("=?" + charset + "?B?" + crypt::base64::encode(val.substr(p)) + "?=");
                }

                i = x;
            }

            if(i != std::string::npos)
                ret += val.substr(i);

            return ret;
        }

        std::string::size_type Headers::find_high(std::string val, std::string::size_type p) {
            for(; p < val.length(); p++) {
                if(static_cast<u_char>(val[p]) > 127) {
                    return p;
                }
            }
            
            return std::string::npos;
        }


        u_int Headers::get_length() const {
            return m_length;
        }

            Headers::iterator Headers::find(std::string key) { return m_map.find(upper(key)); }
            Headers::const_iterator Headers::find(std::string key) const { return m_map.find(upper(key)); }

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

            Headers::list_type::iterator Headers::begin(std::string key) { return m_map.find(upper(key))->second.begin(); }
            Headers::list_type::const_iterator Headers::begin(std::string key) const { return m_map.find(upper(key))->second.begin(); }
            Headers::list_type::iterator Headers::end(std::string key) { return m_map.find(upper(key))->second.end(); }
            Headers::list_type::const_iterator Headers::end(std::string key) const { return m_map.find(upper(key))->second.end(); }
            Headers::list_type::reverse_iterator Headers::rbegin(std::string key) { return m_map.find(upper(key))->second.rbegin(); }
            Headers::list_type::const_reverse_iterator Headers::rbegin(std::string key) const { return m_map.find(upper(key))->second.rbegin(); }
            Headers::list_type::reverse_iterator Headers::rend(std::string key) { return m_map.find(upper(key))->second.rend(); }
            Headers::list_type::const_reverse_iterator Headers::rend(std::string key) const { return m_map.find(upper(key))->second.rend(); }
            bool Headers::empty(std::string key) const { return (m_map.find(upper(key)) == m_map.end() ||
                                                        m_map.find(upper(key))->second.empty()); }
            Headers::size_type Headers::size(std::string key) const { return m_map.find(upper(key))->second.size(); }

            void Headers::clear(std::string key) { if(!empty(upper(key))) m_map.find(upper(key))->second.clear(); }

            std::string Headers::get_charset() { 
                return m_charset;
            }



        
    }
}
