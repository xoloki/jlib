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


#include <jlib/util/util.hh>
#include <jlib/util/URL.hh>
#include <jlib/util/rfc3986.hh>

#include <jlib/util/abnf.hh>

#include <cstdlib>

namespace jlib {
    namespace util {

        namespace {

            /** Built on first use, for the reason at crypt/curve.hh:42. */
            const abnf::grammar& uri_grammar() {
                static abnf::grammar g = [] {
                    abnf::grammar g = abnf::compile(rfc3986::URI_GRAMMAR);
                    g.check();

                    return g;
                }();

                return g;
            }

            abnf::options parse_options() {
                abnf::options o;

                o.captures = abnf::options::capture_policy::listed;
                o.capture_only = { "scheme", "authority", "userinfo", "host", "port",
                                   "path-abempty", "path-absolute",
                                   "path-rootless", "query", "fragment",
                                   "IP-literal" };

                return o;
            }

        }

        URL::URL() {
            
        }

        URL::URL(const std::string& url) {
            parse(url);
        }

        URL::URL(const std::string& protocol, const std::string& host, const std::string& path) {
            set_protocol(protocol);
            set_host(host);
            set_path(path); 
       }

        URL::URL(const std::string& protocol, const std::string& user, const std::string& pass, 
                 const std::string& host, const std::string& port, const std::string& path, const std::string& qs) {
            set_protocol(protocol);
            set_user(user);
            set_pass(pass);
            set_host(host);
            set_port(port);
            set_path(path);
            set_qs(qs);
        }
        
        void URL::parse(const std::string& url) {
            if(std::getenv("JLIB_UTIL_URL_DEBUG"))
                std::cerr << "jlib::util::URL::parse(\""<<url<<"\")"<<std::endl;

            // The trim used to be computed into a local and then never read --
            // every one of the five regexes below it matched against the
            // untrimmed string, and all five were anchored, so a leading space
            // or a trailing newline made a URL unparseable.
            const std::string text = jlib::util::trim(url);

            abnf::match m;

            try {
                m = uri_grammar().at("URI").parse(text, parse_options());
            }
            catch(abnf::budget_exceeded& e) {
                throw exception(std::string("gave up reading a URL: ") + e.what());
            }
            catch(abnf::error& e) {
                throw exception("not a URL at column " + std::to_string(e.column())
                                + "\n  " + e.context_line() + "\n  "
                                + std::string(e.column() - 1, ' ') + "^");
            }

            // RFC 3986 6.2.2.1: the scheme and the host are case insensitive,
            // and lower case is the canonical form.  Every caller in jlib was
            // doing this for itself, with util::lower, at every comparison.
            m_protocol = lower(m["scheme"].str());

            m_authority = static_cast<bool>(m["authority"]);

            const abnf::match user = m["userinfo"];

            m_user.clear();
            m_pass.clear();

            if(user) {
                // The RFC treats userinfo as opaque and deprecates putting a
                // password in it at all (3.2.1), but that is how a mail
                // account is written, and the first colon is where every
                // client splits it.
                const std::string text = user.str();
                const std::string::size_type colon = text.find(':');

                if(colon == text.npos) {
                    m_user = uri::decode(text);
                }
                else {
                    m_user = uri::decode(text.substr(0, colon));
                    m_pass = uri::decode(text.substr(colon + 1));
                }
            }

            const abnf::match host = m["host"];

            m_host_literal = static_cast<bool>(m["IP-literal"]);

            if(m_host_literal) {
                // Without the brackets: they delimit the literal, they are not
                // part of the address, and getaddrinfo does not want them.
                const std::string text = host.str();

                m_host = text.substr(1, text.size() - 2);
            }
            else {
                m_host = lower(uri::decode(host.str()));
            }

            m_port = m["port"] ? m["port"].str() : std::string();

            // hier-part names the path differently depending on which shape it
            // took, and exactly one of the three can be present.
            m_path.clear();

            for(const char* n : { "path-abempty", "path-absolute", "path-rootless" }) {
                if(const abnf::match p = m[n]) {
                    m_path = p.str();
                    break;
                }
            }

            m_fragment = m["fragment"] ? uri::decode(m["fragment"].str()) : std::string();

            set_qs(m["query"] ? m["query"].str() : std::string());
        }

        bool URL::valid(const std::string& url) {
            try {
                URL u(url);
                return true;
            }
            catch(exception&) {
                return false;
            }
        }

        std::map<std::string,std::string> URL::parse_qs(const std::string& qs) {
            std::map<std::string,std::string> ret;

            // Percent-decoded, which it never was: a value written "%20" came
            // back as the three characters.  Split at the *first* "=", because
            // a value may contain one and a name may not.
            std::vector<std::string> tokens = tokenize(qs,"&");
            for(std::vector<std::string>::size_type i=0;i<tokens.size();i++) {
                std::string::size_type j;
                if( (j=tokens[i].find("=")) != std::string::npos ) {
                    ret[uri::decode(tokens[i].substr(0,j))] =
                        uri::decode(tokens[i].substr(j+1));
                }
                else if(!tokens[i].empty()) {
                    // "?flag" with no "=" is a name with an empty value, not
                    // something to drop on the floor.
                    ret[uri::decode(tokens[i])] = std::string();
                }
            }

            return ret;
        }

        std::string URL::parse_qs(std::map<std::string,std::string> qs) {
            std::string ret;

            std::map<std::string,std::string>::iterator i = qs.begin();
            bool first = true;
            for(;i!=qs.end();i++) {
                if(!first) {
                    ret += "&";
                }
                first = false;
                ret += uri::encode(i->first)+"="+uri::encode(i->second);
            }

            return ret;
        }

        std::string URL::get_protocol() const {
            return m_protocol;
        }

        std::string URL::get_user() const {
            return m_user;
        }

        std::string URL::get_pass() const {
            return m_pass;
        }

        std::string URL::get_host() const {
            return m_host;
        }

        std::string URL::get_port() const {
            return m_port;
        }

        std::string URL::get_path() const {
            return m_path;
        }

        std::string URL::get_delim() const {
            return m_delim;
        }

        std::string URL::get_path_no_slash() const {
            std::string::size_type n = m_path.find_first_not_of("/");
            if(n != std::string::npos)
                return m_path.substr(n);
            else 
                return "";
        }

        std::string URL::get_fragment() const {
            return m_fragment;
        }

        std::string URL::get_qs() const {
            return m_qs;
        }
        
        std::map<std::string,std::string> URL::get_qs_hash() const {
            return m_qs_hash;
        }

        unsigned int URL::get_port_val() const {
            return int_value(get_port());
        }
        
        void URL::set_protocol(const std::string& protocol) {
            m_protocol = std::move(protocol);
        }

        void URL::set_user(const std::string& user) {
            m_user = std::move(user);
        }

        void URL::set_pass(const std::string& pass) {
            m_pass = std::move(pass);
        }

        void URL::set_host(const std::string& host) {
            m_host = std::move(host);
        }

        void URL::set_port(const std::string& port) {
            m_port = std::move(port);
        }

        void URL::set_path(const std::string& path) {
            m_path = std::move(path);
        }

        void URL::set_delim(const std::string& delim) {
            m_delim = std::move(delim);
        }

        void URL::set_fragment(const std::string& fragment) {
            m_fragment = std::move(fragment);
        }

        void URL::set_qs(const std::string& qs) {
            m_qs = qs;
            m_qs_hash = parse_qs(qs);
        }

        void URL::set_qs(std::map<std::string,std::string> qs) {
            m_qs_hash = qs;
            m_qs = parse_qs(qs);
        }
        
        std::string URL::operator[](const std::string& key) const {
            const_iterator i = m_qs_hash.find(key);
            if(i != end()) return i->second;
            else return std::string();
        }

        std::string URL::operator()() const {
            return coagulate();
        }

        std::vector<std::string> URL::keys() const {
            std::vector<std::string> ret;

            std::map<std::string,std::string>::const_iterator i = m_qs_hash.begin();
            for(;i!=m_qs_hash.end();i++) {
                std::string key = i->first;
                ret.push_back(key);
            }

            return ret;
        }

        std::string URL::coagulate() const {
            std::string ret = m_protocol + ":";

            if(!m_authority) {
                // "mailto:joe@example.com" -- no "//", and no room for a user,
                // a host or a port either.  RFC 3986 3: the authority is
                // present exactly when the "//" is.
                return ret + m_path + (m_qs.empty() ? "" : "?" + m_qs)
                           + (m_fragment.empty() ? "" : "#" + uri::encode(m_fragment));
            }

            ret += "//";

            // Back the way they came: what parse() decoded, and the brackets
            // it took off a literal.  Without the re-encoding a password
            // containing an "@" would produce a URL that reads back with a
            // different host.
            const std::string host = m_host_literal ? "[" + m_host + "]" : m_host;

            if(m_user != "" && m_pass != "") {
                ret += (uri::encode(m_user)+":"+uri::encode(m_pass)+"@"+host);
            }
            else if(m_user != "") {
                ret += (uri::encode(m_user)+"@"+host);
            }
            else {
                ret += host;
            }

            if(m_port != "") {
                ret += (":"+m_port);
            }

            ret += m_path;

            if(m_qs != "") {
                ret += ("?"+m_qs);
            }

            if(m_fragment != "") {
                ret += ("#"+uri::encode(m_fragment));
            }

            return ret;
        }

        URL::operator std::string() const { return coagulate(); }
     
    }
}

