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

#ifndef JLIB_NET_HEADERS_HH
#define JLIB_NET_HEADERS_HH

#include <exception>
#include <string>
#include <map>
#include <list>
#include <iostream>

namespace jlib {
    namespace util {
        class Headers {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& p_msg = "") {
                    m_msg = "jlib::util::Headers::exception: "+p_msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };


            typedef std::list<std::string> list_type;
            typedef std::list< std::pair<std::string,std::string> > plist_type;
            typedef std::map<std::string, list_type> map_type;

            typedef map_type::pointer pointer;
            typedef map_type::const_pointer const_pointer;
            typedef map_type::reference reference;
            typedef map_type::const_reference const_reference;
            typedef map_type::iterator iterator;
            typedef map_type::const_iterator const_iterator; 
            typedef map_type::reverse_iterator reverse_iterator;
            typedef map_type::const_reverse_iterator const_reverse_iterator;
            typedef map_type::size_type size_type;
            typedef map_type::difference_type difference_type;
            typedef map_type::allocator_type allocator_type;            

            Headers();
            Headers(const std::string& s);
            /**
             * No destructor.
             *
             * There was an empty one, and a user-declared destructor -- even
             * an empty one -- suppresses the implicit move constructor and
             * move assignment.  Every std::move of one of these was silently
             * doing a copy; measured, moving a 2M Email copied 4M.
             */

            std::string operator[](const std::string& key) const;
            operator std::string() const;

            std::string get(const std::string& key) const;
            std::string get(const std::string& key, std::string& charset) const;
            void set(const std::string& key, const std::string& val);
            void add(const std::string& key, const std::string& val);
            void append(const std::string& key, const std::string& val);

            list_type keys() const;
            list_type vals(const std::string& key) const;

            void parse(const std::string& s,bool uppercase=true);

            unsigned int get_length() const;

            iterator find(const std::string& key);
            const_iterator find(const std::string& key) const;

            iterator begin();
            const_iterator begin() const;
            iterator end();
            const_iterator end() const;
            reverse_iterator rbegin();
            const_reverse_iterator rbegin() const;
            reverse_iterator rend();
            const_reverse_iterator rend() const;
            bool empty() const;
            size_type size() const;

            void clear();

            list_type::iterator begin(const std::string& key);
            list_type::const_iterator begin(const std::string& key) const;
            list_type::iterator end(const std::string& key);
            list_type::const_iterator end(const std::string& key) const;
            list_type::reverse_iterator rbegin(const std::string& key);
            list_type::const_reverse_iterator rbegin(const std::string& key) const;
            list_type::reverse_iterator rend(const std::string& key);
            list_type::const_reverse_iterator rend(const std::string& key) const;
            bool empty(const std::string& key) const;
            size_type size(const std::string& key) const;

            void clear(const std::string& key);

            /**
             * RFC 2047, as jlib::util::rfc2047 does it.
             *
             * Kept as the spelling a header caller reaches for; both are one
             * line over jlib/util/encoded_word.hh now.  charset gets the first
             * one seen, which is all a single out-parameter can say -- a
             * header mixing two charsets decodes to a string that no one name
             * describes, and rfc2047::decode's other form hands back all of
             * them.
             */
            static std::string decode(const std::string& val, std::string& charset);
            static std::string encode(const std::string& val, const std::string& charset);

            /**
             * this is from the 'content-type' header
             * e.g. text/plain; charset=ISO-8859-1
             */
            std::string get_charset();


        protected:

            map_type m_map;
            list_type m_keys;
            unsigned int m_length;
            std::string m_charset;
            map_type m_charset_map;
        };
        
    }
}

#endif //JLIB_NET_HEADERS_HH
