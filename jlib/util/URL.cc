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

#include <jlib/util/util.hh>
#include <jlib/util/URL.hh>
#include <jlib/util/Regex.hh>

#include <cstdlib>

const std::string FULL_URL       = "^([[:alnum:]]+)://([[:graph:]]+):([[:print:]]+)@([[:graph:]]+):([[:digit:]]+)(\\/.*)\\?(.*)$";
const std::string BASIC_URL      = "^([[:alnum:]]+)://([[:graph:]]+)$";
const std::string HOST_WITH_PATH = "^([^/ ]*)(\\/.*)$";
const std::string HOST_WITH_USER = "^([[:graph:]]+)@([[:graph:]]+)$";
const std::string HOST_WITH_PORT = "^([[:graph:]]+):([[:digit:]]+)$";
const std::string USER_WITH_PASS = "^([[:graph:]]+):([[:graph:]]+)$";
const std::string PATH_WITH_QS   = "^(\\/.+)\\?(.*)$";

namespace jlib {
    namespace util {

        URL::URL() {
            
        }

        URL::URL(std::string url) {
            parse(url);
        }

        URL::URL(std::string protocol, std::string host, std::string path) {
            set_protocol(protocol);
            set_host(host);
            set_path(path); 
       }

        URL::URL(std::string protocol, std::string user, std::string pass, 
                 std::string host, std::string port, std::string path, std::string qs) {
            set_protocol(protocol);
            set_user(user);
            set_pass(pass);
            set_host(host);
            set_port(port);
            set_path(path);
            set_qs(qs);
        }
        
        URL::~URL() {
            
        }
        
        void URL::parse(std::string url) {
            if(std::getenv("JLIB_UTIL_URL_DEBUG"))
                std::cerr << "jlib::util::URL::parse(\""<<url<<"\")"<<std::endl;
            
            url = jlib::util::trim(url);
            jlib::util::Regex full_url(FULL_URL);
            
            if(full_url(url)) {
                set_protocol(full_url[1]);
                set_user(full_url[2]);
                set_pass(full_url[3]);
                set_host(full_url[4]);
                set_port(full_url[5]);
                set_path(full_url[6]);
                set_qs(full_url[7]);
            }
            else {
                jlib::util::Regex basic_url(BASIC_URL);                

                if(basic_url(url)) {
                    set_protocol(basic_url[1]);

                    std::string host = basic_url[2];

                    jlib::util::Regex host_with_path(HOST_WITH_PATH);

                    if(host_with_path(host)) {
                        host = host_with_path[1];
                        std::string path = host_with_path[2];

                        jlib::util::Regex path_with_qs(PATH_WITH_QS);

                        if(path_with_qs(path)) {
                            path = path_with_qs[1];
                            std::string qs = path_with_qs[2];
                            set_qs(qs);
                        }

                        set_path(path);
                    }

                    jlib::util::Regex host_with_user(HOST_WITH_USER);
                    
                    if(host_with_user(host)) {
                        std::string user = host_with_user[1];
                        host = host_with_user[2];
                        
                        jlib::util::Regex user_with_pass(USER_WITH_PASS);

                        if(user_with_pass(user)) {
                            user = user_with_pass[1];
                            std::string pass = user_with_pass[2];
                            set_pass(pass);
                        }
                        
                        set_user(user);
                    }

                    jlib::util::Regex host_with_port(HOST_WITH_PORT);

                    if(host_with_port(host)) {
                        host = host_with_port[1];
                        std::string port = host_with_port[2];
                        set_port(port);
                    }

                    set_host(host);
                    
                }
                else {
                    throw exception("failed to match basic URL syntax: "+
                                    BASIC_URL+" at jlib::util::URL::parse, passed string \""+url+"\"");
                }
                
                
            }
            
        }

        std::map<std::string,std::string> URL::parse_qs(std::string qs) {
            std::map<std::string,std::string> ret;

            std::vector<std::string> tokens = tokenize(qs,"&");
            for(std::vector<std::string>::size_type i=0;i<tokens.size();i++) {
                std::string::size_type j;
                if( (j=tokens[i].find("=")) != std::string::npos ) {
                    ret[tokens[i].substr(0,j)] = tokens[i].substr(j+1);
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
                ret += i->first+"="+i->second;
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

        std::string URL::get_qs() const {
            return m_qs;
        }
        
        std::map<std::string,std::string> URL::get_qs_hash() const {
            return m_qs_hash;
        }

        unsigned int URL::get_port_val() const {
            return int_value(get_port());
        }
        
        void URL::set_protocol(std::string protocol) {
            m_protocol = protocol;
        }

        void URL::set_user(std::string user) {
            m_user = user;
        }

        void URL::set_pass(std::string pass) {
            m_pass = pass;
        }

        void URL::set_host(std::string host) {
            m_host = host;
        }

        void URL::set_port(std::string port) {
            m_port = port;
        }

        void URL::set_path(std::string path) {
            m_path = path;
        }

        void URL::set_delim(std::string delim) {
            m_delim = delim;
        }

        void URL::set_qs(std::string qs) {
            m_qs = qs;
            m_qs_hash = parse_qs(qs);
        }

        void URL::set_qs(std::map<std::string,std::string> qs) {
            m_qs_hash = qs;
            m_qs = parse_qs(qs);
        }
        
        std::string URL::operator[](std::string key) const {
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
            std::string ret = m_protocol + "://";

            if(m_user != "" && m_pass != "") {
                ret += (m_user+":"+m_pass+"@"+m_host);
            }
            else if(m_user != "") {
                ret += (m_user+"@"+m_host);
            }
            else {
                ret += m_host;
            }

            if(m_port != "") {
                ret += (":"+m_port);
            }

            ret += m_path;

            if(m_qs != "") {
                ret += ("?"+m_qs);
            }

            return ret;
        }

        URL::operator std::string() const { return coagulate(); }
     
    }
}

