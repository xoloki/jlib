/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
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
        
        
        void parse_divide(std::istream& is, std::vector<long>& divide, const std::string& div);

        /**
         * Make a body safe to send inside a dot-terminated stream.
         *
         * RFC 5321 4.5.2, and RFC 1939 3 says the same thing from the other
         * end: a line beginning with "." gets a second "." in front of it, so
         * that the "." on a line by itself which ends the body can only ever
         * be the end of the body.  Both protocols call it transparency and
         * both mean this; it is one function because it is one mechanism.
         *
         * Expects CRLF line endings -- run convert_to_crlf() first.
         *
         * By value deliberately: data is modified and returned.
         */
        std::string dot_stuff(std::string data);

        /**
         * Undo dot_stuff() on a body just received.
         *
         * The terminating "." is not part of the body and is not expected
         * here; Pop3::read_body() takes it off.
         *
         * By value deliberately: data is modified and returned.
         */
        std::string dot_unstuff(std::string data);

        /**
         * Are these two the same person?
         *
         * Both sides are parsed and compared without regard to case.  Anything
         * that cannot be read is not the same as anything -- see the note in
         * net.cc for what that used to answer instead.
         */
        bool same_address(const std::string& p_addr1, const std::string& p_addr2);

        /**
         * The address out of "Joe Yandle <joe@x.com> (at home)".
         *
         * mailbox::parse(s, lenient()).addr().str(), and kept because that is
         * a mouthful for the commonest thing a caller wants.  Throws
         * address::exception on anything it cannot read; it does not return
         * an empty string.
         *
         * extract_addresses() and split_addresses() are gone.  jlib::net's
         * mailbox::parse_list does both, and does them against the RFC's
         * grammar: addr().str() for the values, source() for the text each one
         * was written as.  See jlib/net/address.hh.
         */
        std::string extract_address(const std::string& p_addr);

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
            // Request, Response and request() were declared here and defined
            // nowhere -- no constructor, no get_text(), no request(), in any
            // file in the tree.  Twenty-odd years of a header promising an HTTP
            // client that did not exist; the only thing here that ever ran is
            // get(), below.  They are gone rather than kept as a sketch,
            // because the sketch had already started to constrain the real
            // thing: Request held a util::Headers, which is an RFC 5322 header
            // set that decodes encoded-words, and HTTP fields are not that.
            //
            // get() is HTTP/1.0, plaintext-only, and treats any status but 200
            // as an error, so it cannot follow a redirect or read the body of a
            // 400 -- which is precisely what an OAuth2 token endpoint answers
            // with when it has something to tell you.  Nothing in the tree
            // calls it.  It goes when there is something to replace it with.
            std::string get(jlib::util::URL url);
        }

        namespace html {
            std::string render(const std::string& s);
        }

    }
}

#endif //JLIB_NET_HH
