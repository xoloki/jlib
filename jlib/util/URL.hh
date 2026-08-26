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

            /**
             * Read a URI, against RFC 3986's own grammar.
             *
             * Throws URL::exception, with a column, on anything that is not
             * one.  A scheme is required: "example.com/x" is a relative
             * reference and jlib has no base to resolve it against.
             *
             * The scheme and the host are lowercased, which RFC 3986 6.2.2.1
             * says is the canonical form and which every caller here was
             * doing for itself.  The userinfo is percent-decoded, so
             * "joe%40example.com" comes back as "joe@example.com"; the path
             * and the query are not, because "%2F" in a path is a character
             * and "/" is a separator and decoding loses the difference.
             */
            void parse(const std::string& url);

            /**
             * Parse an RFC 3986 4.1 URI-reference: a URI, or a relative one.
             *
             * Separate from parse() on purpose, rather than parse() being
             * widened.  A mail URL with a typo in it -- "imap:/host", say --
             * is a perfectly good relative reference, so a parse() that
             * accepted one would stop throwing on it and start returning
             * something with no scheme and no host, which Imap4 would then try
             * to connect to.  Refusing at the point of parse is worth keeping.
             *
             * What wants this is HTTP: RFC 9110 makes Location a
             * URI-reference, and a 302 pointing at "/signin" is the ordinary
             * case rather than the exotic one.
             *
             * Nothing here resolves a reference against a base.  RFC 3986 5.3
             * is an algorithm of its own and no caller has needed it yet;
             * relative() is how to find out that you have one.
             */
            void parse_reference(const std::string& url);

            /** Would it parse as a URI?  The way to ask without catching. */
            static bool valid(const std::string& url);

            /** Would it parse as a URI-reference? */
            static bool valid_reference(const std::string& url);

            /** No scheme: this came from parse_reference() and is relative. */
            bool relative() const { return m_protocol.empty(); }

            /**
             * Split a query string into its pairs, percent-decoding both
             * sides.
             *
             * "+" is not a space here.  That convention belongs to HTML's
             * application/x-www-form-urlencoded, not to RFC 3986, where "+"
             * is a sub-delim that stands for itself -- and a URL class that
             * silently turned it into a space would corrupt every base64
             * value anyone put in a query.
             */
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

            /** The "#fragment", without its "#", or "" when there was none. */
            std::string get_fragment() const;

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
            void set_fragment(const std::string& fragment);
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
            std::string m_fragment;

            /**
             * Whether the URI had an authority -- a "//" after the scheme.
             *
             * Not the same as having a host: "mbox:///home/joe/Mail" has an
             * authority and it is empty, while "mailto:joe@example.com" has
             * none at all, and coagulate() has to put back what was there.
             * True by default so that a URL built through a constructor,
             * which takes a host, still writes one.
             */
            bool m_authority = true;

            /**
             * Whether the host was written as "[...]".
             *
             * Kept so that get_host() can hand back the bare address -- which
             * is what getaddrinfo wants -- and coagulate() can put the
             * brackets back.
             */
            bool m_host_literal = false;

            std::string m_delim;
            std::map<std::string,std::string> m_qs_hash;

        protected:
            void parse(const std::string& url, const char* start);

        };
        
    }
}

#endif //JLIB_NET_URL_HH
