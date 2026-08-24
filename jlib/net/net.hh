/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_NET_HH
#define JLIB_NET_HH

#include <string>
#include <iostream>
#include <map>
#include <vector>
#include <exception>

#include <jlib/util/URL.hh>
#include <jlib/util/Headers.hh>
#include <jlib/net/Email.hh>

#include <jlib/sys/socketstream.hh>

namespace jlib {
    /**
     * Namespace jlib::net
     *
     */
	namespace net {
        class exception : public std::exception {
        public:
            exception(const std::string& msg = "") {
                m_msg = (msg != "" ? ("jlib::net::exception: "+msg) : "jlib::net::exception");
            }
            virtual ~exception() {}
            virtual const char* what() const noexcept { return m_msg.c_str(); }
        protected:
            std::string m_msg;
        };
        
        
        void parse_headers(std::istream& is, std::multimap<std::string,std::string>& m, bool uppercase=true);

        /**
         * get a vector of the headers whose key is header
         */
        std::vector<std::string> parse_header(std::istream& stream, const std::string& header);

        void parse_divide(std::istream& is, std::vector<long>& divide, const std::string& div);

        /**
         * parse through s, looking for the first occurance of something in ends
         * when it's found, return the std::string up to that point
         */
        std::string parse_end(const std::string& s, const std::vector<std::string>& ends);

        /**
         * parse through is, looking for the first occurance of something in ends
         * when it's found, copy the std::string up to that point into raw, then set 
         * the streampos to the end (actually, the beginning of the next line)
         */
        void parse_end(std::istream& is, std::vector<std::string> ends, std::string& raw);

        /**
         * Are the two email addresses the same, regardless of style, caps, etc?
         *
         * @param p_addr1 first address
         * @param p_addr1 second address
         * @return true if same addr, ow false
         */
        bool same_address(const std::string& p_addr1, const std::string& p_addr2);
            
        /**
         * Extract the actual email address from the given string
         * 
         * @param p_addr passed email address
         * @return extracted email addr
         * @throw email_exception if we can't parse a valid address from the string
         */
        std::string extract_address(const std::string& p_addr);

        /**
         * Extract the actual email addresses from a comma delimited string
         * 
         * @param s string of passed email address
         * @return list of extracted email addrs
         * @throw email_exception if we can't parse a valid address from the string
         */
        std::list<std::string> extract_addresses(const std::string& s);

        /**
         * Split the full addresses from a comma delimited string
         * 
         * @param s string of passed email address
         * @return list of split email addrs
         * @throw email_exception if we can't parse a valid address from the string
         */
        std::list<std::string> split_addresses(const std::string& s);

        /**
         * recurse through email, building text for mime and placing it
         * into data.
         * 
         */
        void build_mime(std::string& data, Email& email, bool is_recurse=false);

        /**
         * Do a lookup on the passed name/addr
         */
        std::pair< std::string, std::vector<std::string> > get_host(const std::string& s);

        bool is_addr(const std::string& s);

        std::string get_ip_string(long addr);
        long get_ip_val(const std::string& addr);

        bool is_reserved(const std::string& ip);

        std::string pathstr(std::list<std::string> path, 
                            const std::string& delim="/", 
                            bool begin_delim = true, 
                            bool end_delim = false,
                            bool only_delim = false);


        namespace mbox {
            std::string make_path(const std::string& maildir, std::list<std::string> path);

            void create(const std::string& maildir, std::list<std::string> path);
            void create(const std::string& path);

            void deleet(const std::string& maildir, std::list<std::string> path);
            void deleet(const std::string& path);

            void rename(const std::string& maildir, std::list<std::string> src, std::list<std::string> dst);
            void rename(const std::string& src, const std::string& dst);

            void remove(const std::string& maildir, std::list<std::string> path, std::list<int> which, std::vector<long> divide);
            void remove(const std::string& path, std::list<int> which, std::vector<long> divide);

            Email get(const std::string& maildir, std::list<std::string> path, int i, std::vector<long> divide, bool oheader=false);
            Email get(const std::string& path, int i, std::vector<long> divide, bool oheader=false);
            Email get(std::istream& is, int i, std::vector<long> divide, bool oheader=false);

            void append(const std::string& maildir, std::list<std::string> path, Email e);
            void append(const std::string& path, Email e);
        }

        namespace smtp {
            void send(const std::string& mail, const std::string& rcpt, const std::string& data, const std::string& host, unsigned int port);

            void send_ssl(const std::string& mail, const std::string& rcpt, const std::string& data, const std::string& host, unsigned int port);

            void send_tls(const std::string& mail, const std::string& rcpt, const std::string& data, const std::string& host,unsigned int port);
            void send_tls_auth(const std::string& mail, const std::string& rcpt, const std::string& data, const std::string& host,unsigned int port, const std::string& user, const std::string& pass);
            void send_ssl_auth(const std::string& mail, const std::string& rcpt, const std::string& data, const std::string& host,unsigned int port, const std::string& user, const std::string& pass);
        }

        namespace http {
            typedef enum { v10, v11 } version;
            
            class Request {
            public:
                typedef enum { get, head, post, put, connect } method;

                Request(method m, util::URL u);
                
                version m_version;
                method m_method;
                util::URL m_url;
                util::Headers m_headers;
                std::string m_data;
            };

            class Response {
            public:
                std::string get_text() const;
                static std::string get_text(int status);

                int m_status;
                version m_version;
                util::Headers m_headers;
                std::string m_data;
            };

            std::string get(jlib::util::URL url);
            
            Response request(const Request& r);

        }

        namespace html {
            std::string render(const std::string& s);
        }

    }
}

#endif //JLIB_NET_HH
