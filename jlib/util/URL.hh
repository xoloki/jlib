/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 2001 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_NET_URL_HH
#define JLIB_NET_URL_HH

#include <exception>
#include <string>
#include <map>
#include <vector>

namespace jlib {
    namespace util {
        class URL {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& p_msg = "") {
                    m_msg = "jlib::util::URL::exception: "+p_msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            typedef std::map<std::string,std::string> rep_type;

            typedef rep_type::pointer pointer;
            typedef rep_type::const_pointer const_pointer;
            typedef rep_type::reference reference;
            typedef rep_type::const_reference const_reference;
            typedef rep_type::iterator iterator;
            typedef rep_type::const_iterator const_iterator; 
            typedef rep_type::reverse_iterator reverse_iterator;
            typedef rep_type::const_reverse_iterator const_reverse_iterator;
            typedef rep_type::size_type size_type;
            typedef rep_type::difference_type difference_type;
            typedef rep_type::allocator_type allocator_type;            

            URL();
            URL(const std::string& url);
            URL(const std::string& protocol, const std::string& host, const std::string& path);
            URL(const std::string& protocol, const std::string& user, const std::string& pass, 
                const std::string& host, const std::string& port, const std::string& path, const std::string& qs);
            /**
             * No destructor.
             *
             * There was an empty one, and a user-declared destructor -- even
             * an empty one -- suppresses the implicit move constructor and
             * move assignment.  Every std::move of one of these was silently
             * doing a copy; measured, moving a 2M Email copied 4M.
             */

            void parse(const std::string& url);
            static std::map<std::string,std::string> parse_qs(const std::string& qs);
            static std::string parse_qs(std::map<std::string,std::string> qs);

            std::string get_protocol() const;
            std::string get_user() const;
            std::string get_pass() const;
            std::string get_host() const;
            std::string get_port() const;
            std::string get_path() const;
            std::string get_path_no_slash() const;

            std::string get_delim() const;

            std::string get_qs() const;
            std::map<std::string,std::string> get_qs_hash() const;

            unsigned int get_port_val() const;

            std::string operator[](const std::string& key) const;
            std::string operator()() const;
            operator std::string() const;
            std::vector<std::string> keys() const;

            void set_protocol(const std::string& protocol);
            void set_user(const std::string& user);
            void set_pass(const std::string& pass);
            void set_host(const std::string& host);
            void set_port(const std::string& port);
            void set_path(const std::string& path);
            void set_delim(const std::string& delim);
            void set_qs(const std::string& qs);
            void set_qs(std::map<std::string,std::string> qs);
           
            std::string coagulate() const;

            iterator begin() { return m_qs_hash.begin(); }
            const_iterator begin() const { return m_qs_hash.begin(); }
            iterator end() { return m_qs_hash.end(); }
            const_iterator end() const { return m_qs_hash.end(); }
            reverse_iterator rbegin() { return m_qs_hash.rbegin(); }
            const_reverse_iterator rbegin() const { return m_qs_hash.rbegin(); }
            reverse_iterator rend() { return m_qs_hash.rend(); }
            const_reverse_iterator rend() const { return m_qs_hash.rend(); }
            bool empty() const { return m_qs_hash.empty(); }
            size_type size() const { return m_qs_hash.size(); }

        protected:

            std::string m_protocol;
            std::string m_user;
            std::string m_pass;
            std::string m_host;
            std::string m_port;
            std::string m_path;
            std::string m_qs;
            std::string m_delim;
            std::map<std::string,std::string> m_qs_hash;
        };
        
    }
}

#endif //JLIB_NET_URL_HH
