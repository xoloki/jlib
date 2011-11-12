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
                exception(std::string p_msg = "") {
                    m_msg = "jlib::util::Headers::exception: "+p_msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
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
            Headers(std::string s);

            virtual ~Headers();

            std::string operator[](std::string key) const;
            operator std::string() const;

            std::string get(std::string key) const;
            std::string get(std::string key, std::string& charset) const;
            void set(std::string key, std::string val);
            void add(std::string key, std::string val);
            void append(std::string key, std::string val);

            list_type keys() const;
            list_type vals(std::string key) const;

            void parse(std::string s,bool uppercase=true);

            unsigned int get_length() const;

            iterator find(std::string key);
            const_iterator find(std::string key) const;

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

            list_type::iterator begin(std::string key);
            list_type::const_iterator begin(std::string key) const;
            list_type::iterator end(std::string key);
            list_type::const_iterator end(std::string key) const;
            list_type::reverse_iterator rbegin(std::string key);
            list_type::const_reverse_iterator rbegin(std::string key) const;
            list_type::reverse_iterator rend(std::string key);
            list_type::const_reverse_iterator rend(std::string key) const;
            bool empty(std::string key) const;
            size_type size(std::string key) const;

            void clear(std::string key);

            static std::string decode(std::string val, std::string& charset);
            static std::string encode(std::string val, std::string charset);
            static std::string::size_type find_high(std::string val, std::string::size_type p);

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
