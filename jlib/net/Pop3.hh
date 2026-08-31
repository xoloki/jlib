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

#ifndef JLIB_POP3_HH
#define JLIB_POP3_HH

#include <jlib/net/Email.hh>

#include <jlib/sys/socketstream.hh>

#include <jlib/util/URL.hh>

#include <exception>
#include <list>
#include <string>

namespace jlib {
    namespace net {

        /**
         * Class POP3 retrieves email from a POP3 server
         */
        class Pop3 {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = "jlib::net::Pop3::exception: "+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };
        

            Pop3(jlib::util::URL url, bool remove=true);

            /**
             * How long a read or write may block, in seconds; 0 waits forever.
             *
             * **Forever is the default, and it is the wrong thing to leave it
             * at.** A server that accepts the connection and then says nothing
             * -- because it is wedged, or because it is waiting for a TLS
             * handshake this end is not going to send -- leaves the client
             * blocked with no way out. Nothing here notices; the connection is
             * open and the read simply never returns.
             *
             * That is not hypothetical. Pointing a STARTTLS session at an
             * implicit-TLS port is exactly the case, and it cost the test suite
             * three minutes per occurrence: each side waited for the other
             * until *dovecot's* own login timeout closed the socket. Against a
             * server with no such timeout it would have waited indefinitely.
             *
             * The default is unchanged rather than fixed because changing it
             * changes behaviour for every existing caller; see the issue.
             */
            void set_timeout(double seconds) { m_timeout = seconds; }
            double get_timeout() const { return m_timeout; }


            /**
             * Does this URL's scheme mean TLS?
             *
             * "pop3s" and "spop", compared whole.  It used to be
             * find("spop") != npos, and "pop3s" does not contain "spop" -- so
             * the standard scheme chose port 110 and a plain socket.
             */
            static bool is_secure(const jlib::util::URL& url);

            /**
             * Was STARTTLS asked for?
             *
             *     pop3://mail.example.com/?tls=starttls
             *
             * RFC 2595 4 calls the command STLS: connect in the clear on the
             * ordinary port, then upgrade in place.  A query parameter rather
             * than a third scheme, matching Imap4.
             *
             * If it is asked for and the server does not offer it, connect()
             * throws rather than carrying on in the clear.
             */
            static bool use_starttls(const jlib::util::URL& url);

            /**
             * Retrieve all email
             */
            /**
             * Every message in the maildrop, as raw text.
             *
             * Deletes each one after reading it unless the constructor was
             * told otherwise -- which is what POP3 is for, and what the
             * `remove` flag has always claimed to control.
             */
            std::list<std::string> retrieve();

            /**
             * Read a multi-line response body, up to the "." that ends it.
             *
             * RFC 1939 3: a multi-line response ends with CRLF "." CRLF, and
             * nothing else terminates it.  The dot transparency is undone on
             * the way past, so what comes back is the message as it was sent.
             *
             * Public and static because it is the whole of what is worth
             * testing here, and a std::istringstream is a POP3 server for the
             * purpose.
             *
             * Throws Pop3::exception if the stream ends before the "." does --
             * a truncated message is not a short message.
             */
            static std::string read_body(std::istream& is);

        protected:
            /**
             * The capabilities the server advertises.  RFC 2449, CAPA.
             *
             * Empty when the server does not implement CAPA at all, which is
             * legal -- it postdates RFC 1939 -- and which is why asking for
             * STARTTLS against such a server is an error rather than a guess.
             */
            std::list<std::string> capa(jlib::sys::socketstream& sock);

            /** Negotiate STLS on an already-connected plaintext stream. */
            void upgrade(jlib::sys::socketstream& sock);

            /** How many messages the maildrop holds.  RFC 1939 5, STAT. */
            unsigned int count(jlib::sys::socketstream& sock);

            std::string retrieve(jlib::sys::socketstream& sock, unsigned int which);
            
            void remove(jlib::sys::socketstream& sock,unsigned int which);
            
            /**
             * Connect to the server
             */
            jlib::sys::socketstream* connect();
            
            /**
             * Disconnect from the server
             */
            void disconnect(jlib::sys::socketstream& sock);
            
            std::string handshake(jlib::sys::socketstream& sock, const std::string& data, const std::string& ok);
            
            jlib::util::URL m_url;

            /** Seconds a read or write may block; 0 is forever. */
            double m_timeout = 0;
            bool m_remove;
        };
        
    }
}
#endif //JLIB_POP3_HH
