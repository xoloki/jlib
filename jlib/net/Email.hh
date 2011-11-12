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

#ifndef JLIB_NET_EMAIL_HH
#define JLIB_NET_EMAIL_HH

#include <fstream>
#include <map>
#include <vector>
#include <exception>
#include <string>
#include <set>

#include <jlib/util/Headers.hh>

namespace jlib {
    namespace net {
        /**
         * Class Email
         */
        class Email {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = "jlib::net::email exception: "+msg;
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };


            enum flag_type { answered_flag, deleted_flag, seen_flag };
            /*
            typedef unsigned int flag_type;

            static const flag_type answered_flag = 0;
            static const flag_type deleted_flag = 1;
            static const unsigned int seen_flag = 2;
            */

            typedef std::vector<Email> rep_type;

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

            /**
             * Default constructor.
             */
            Email();

            /**
             * create email from passed data string
             */
            Email(std::string is);
            
            void create(std::string is);

            /**
             * Destructor.
             */
            ~Email();
            
            void set(std::string key,std::string val);
            void add(std::string key,std::string val);

            std::string find(std::string key) const { return operator[](key); }
            std::string operator[](std::string key) const;
            Email& operator[](unsigned int i) { return m_attach[i]; }

            void push_front(const Email& e) { m_attach.insert(m_attach.begin(),e); }
            void push_back(const Email& e) { m_attach.insert(m_attach.end(),e); }
            
            /**
             * Get the raw text of this email.
             *
             * @return raw text
             */
            std::string raw() const { return m_raw; }

            /**
             * Get vector of Email attachments
             *
             * @return vector<Email> of attachments
             */
            std::vector<Email>& attach();
            
            /**
             * Tell is this email is an internal folder data email
             *
             * @return true if email is internal data, ow false
             */ 
            bool internal() const;
            
            /**
             * Inequality operator, used for sorting
             *
             * @param j1 first Email 
             * @param j2 next Email 
             * @return true if j1 should come before j2, ow false
             */ 
            friend bool operator<(const Email& j1, const Email& j2);
            
            /**
             * Set the field to sort by
             *
             * @param field field to sort by
             *
             */
            void sort(std::string field);
            
            /**
             * Get the headers for this email.
             *
             * @return headers
             */
            jlib::util::Headers& headers() { return m_headers; }

            /**
             * set the binary data to what is passed
             */
            void data(std::string data) { m_data = data; }
            
            /**
             * get the binary data
             */
            std::string data() const { return m_data; }

            /**
             * build the text of the email based on the data and attachments
             * (and their data, and their...)
             */
            void build();

            /**
             * set the flags to the passed set
             */
            std::set<flag_type> get_flags() { return m_flags; }

            void set_flags(std::set<flag_type> flags);
            void unset_flags(std::set<flag_type> flags);

            void set_flag(flag_type flag);
            void unset_flag(flag_type flag);

            void clear_flags();

            iterator begin() { return m_attach.begin(); }
            const_iterator begin() const { return m_attach.begin(); }
            iterator end() { return m_attach.end(); }
            const_iterator end() const { return m_attach.end(); }
            reverse_iterator rbegin() { return m_attach.rbegin(); }
            const_reverse_iterator rbegin() const { return m_attach.rbegin(); }
            reverse_iterator rend() { return m_attach.rend(); }
            const_reverse_iterator rend() const { return m_attach.rend(); }
            bool empty() const { return m_attach.empty(); }
            size_type size() const { return m_attach.size(); }

            unsigned int get_data_size() const { return m_data_size; }
            void set_data_size(unsigned int size) { m_data_size = size; }

            std::vector<std::string> get_received() const { return m_received; }
            std::string get_received_ip() const;

            std::string get_primary_text() const;
            std::string get_primary_text_html() const;
            std::string get_primary_text_html_render() const;

            std::string get_globbed_text() const;
            std::string get_globbed_text_html() const;
            std::string get_globbed_text_html_render() const;
            
            std::string get_name() const;
            std::string get_filename() const;

            bool is(std::string type) const;

            void parse_received();

            static bool is_valid(char c);

            bool is_loaded() const;
            void set_loaded(bool b);

            void set_indx(int indx);
            int get_indx() const;

            reference grep(std::string s, bool recursive = true);

        protected:
            std::string get_text(bool html, bool render, bool globbed, bool recurse) const;
            bool check(std::string buf);
            
            std::string m_sort;
            std::string m_raw;
            
            std::vector<std::string> m_bounds;
            std::vector<Email> m_attach;

            jlib::util::Headers m_headers;
            std::string m_data;
            std::set<flag_type> m_flags;

            unsigned int m_data_size;

            std::vector<std::string> m_received;
            bool m_is_loaded;

            int m_indx;
        };
        
    }
}

#endif //JLIB_NET_EMAIL_HH
