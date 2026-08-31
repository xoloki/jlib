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

#include <jlib/net/Pop3.hh>
#include <jlib/net/net.hh>

#include <jlib/sys/sys.hh>
#include <jlib/sys/sslstream.hh>
#include <jlib/sys/sslproxystream.hh>

#include <jlib/util/util.hh>

#include <cctype>
#include <sstream>
#include <memory>

const int PORT = 110;
const int SPORT = 995;
const std::string OK = "+OK";

namespace jlib {
    namespace net {
        
        bool Pop3::is_secure(const jlib::util::URL& url) {
            // A whole-scheme comparison, not find().  "pop3s" contains "pop"
            // and does not contain "spop", so the old test read the standard
            // scheme as *plain* POP3: it chose port 110, opened a plain
            // socketstream, and sent the password over it.  Silently, and for
            // the one spelling anybody outside this library would use.
            //
            // Both spellings are here: "pop3s" is what IANA registered,
            // "spop" is the older convention this library was written with.
            const std::string scheme = jlib::util::lower(url.get_protocol());

            return scheme == "pop3s" || scheme == "spop" || scheme == "spop3";
        }

        bool Pop3::use_starttls(const jlib::util::URL& url) {
            return jlib::util::lower(url["tls"]) == "starttls";
        }

        Pop3::Pop3(jlib::util::URL url, bool remove) 
            : m_remove(remove)
        {
            m_url = url;

            const std::string scheme = jlib::util::lower(m_url.get_protocol());

            if(!is_secure(m_url) && scheme != "pop3" && scheme != "pop") {
                throw exception("bad protocol in jlib::net::Pop3::Pop3(), m_url = "+m_url());
            }

            if(m_url.get_port() == "") {
                m_url.set_port(jlib::util::string_value(is_secure(m_url) ? SPORT : PORT));
            }
        }
        
        
        std::list<std::string> Pop3::capa(jlib::sys::socketstream& sock) {
            std::list<std::string> ret;

            try {
                handshake(sock, "CAPA", OK);
            }
            catch(exception&) {
                // RFC 2449 postdates RFC 1939 and a server need not implement
                // it.  An empty list rather than an error -- the caller
                // decides what not knowing means.
                return ret;
            }

            // A multi-line response, terminated by "." on a line of its own,
            // exactly as a message body is.
            std::istringstream body(read_body(sock));
            std::string line;

            while(std::getline(body, line)) {
                if(!line.empty() && line.back() == '\r') line.pop_back();
                if(!line.empty()) ret.push_back(line);
            }

            return ret;
        }

        void Pop3::upgrade(jlib::sys::socketstream& sock) {
            // RFC 2595 4.
            bool offered = false;

            for(const std::string& c : capa(sock)) {
                if(jlib::util::upper(c) == "STLS") offered = true;
            }

            if(!offered) {
                throw exception("the server does not offer STLS, and "
                                "?tls=starttls asked for it");
            }

            handshake(sock, "STLS", OK);

            jlib::sys::tlsstream* tls = dynamic_cast<jlib::sys::tlsstream*>(&sock);

            if(tls == 0) {
                throw exception("STLS on a stream that cannot be upgraded");
            }

            // The same verification an implicit-TLS connection gets:
            // SSL_VERIFY_PEER, the default trust store, and SSL_set1_host on
            // the name that was connected to.
            tls->start();

            // 2595 4: the client must discard what CAPA said before the
            // handshake.  Everything read then was unauthenticated, so a man
            // in the middle could have taken STLS out of that list, or put an
            // authentication mechanism into it.
            capa(sock);
        }

        unsigned int Pop3::count(jlib::sys::socketstream& sock) {
            // RFC 1939 5: "+OK nn mm" -- the number of messages and the size
            // of the maildrop.  Read as a number rather than as tokenize()[1],
            // so that a server answering "+OK" with nothing after it is an
            // error here rather than an out-of-range index somewhere later.
            const std::string stat = handshake(sock, "STAT", OK);
            std::string::size_type i = stat.find_first_of("0123456789");

            if(i == stat.npos) {
                throw exception("no message count in the STAT reply: " + stat);
            }

            unsigned int n = 0;

            for(; i < stat.size() && std::isdigit(static_cast<unsigned char>(stat[i])); i++) {
                n = n * 10 + static_cast<unsigned int>(stat[i] - '0');
            }

            return n;
        }

        std::list<std::string> Pop3::retrieve() {
            // This connected, disconnected and returned an empty list: the
            // loop was commented out, so the only public way to get mail out
            // of a POP3 account has never got any.  It reported success while
            // doing it, which is why nobody noticed.
            std::list<std::string> ret;
            std::unique_ptr<jlib::sys::socketstream> sock(connect());

            const unsigned int n = count(*sock);

            for(unsigned int i = 1; i <= n; i++) {
                ret.push_back(retrieve(*sock, i));

                if(m_remove) remove(*sock, i);
            }

            disconnect(*sock);

            return ret;
        }

        std::string Pop3::read_body(std::istream& is) {
            std::string body, line;

            for(;;) {
                if(!std::getline(is, line)) {
                    throw exception("connection ended before the \".\" that ends "
                                    "a multi-line response");
                }

                // Exactly one CRLF comes off, not every trailing CR.
                // sys::getline() erases all of them, which is right for a
                // command response and wrong for message content: a body line
                // that genuinely ends in CR would come back a byte short and
                // the message would no longer be what was sent.
                if(!line.empty() && line.back() == '\r') line.pop_back();

                if(line == ".") return dot_unstuff(std::move(body));

                body += line;
                body += "\r\n";
            }
        }

        std::string Pop3::retrieve(jlib::sys::socketstream& sock, unsigned int which) {
            // Read to the terminating ".", not to the octet count in the +OK.
            //
            // This used to tokenize the "+OK 1234 octets" line and read
            // exactly 1234 bytes.  That count is optional in RFC 1939 -- the
            // response is "+OK message follows" and the size is a courtesy --
            // and where it is sent it is a maildrop size that counts CRLF as
            // one octet on some servers and two on others.  Reading the wrong
            // number leaves the rest of the message sitting in the socket, and
            // every command after it reads somebody else's mail as its
            // response.  On a server that omits the number entirely,
            // bufvec[1] was out of range.
            handshake(sock, "RETR " + jlib::util::string_value(which), OK);

            return read_body(sock);
        }
        
        jlib::sys::socketstream* Pop3::connect() {
            jlib::sys::socketstream* sock;

            // The URL's ?proxy= parameter, which this read nowhere at all: it
            // was accepted by the URL parser, stored, and silently ignored, so
            // pop3s://host/?proxy=p:3128 connected direct.  A caller asking to
            // go through a proxy may be doing so because it is the only route
            // out; giving them a direct connection instead is the worst of the
            // three things that could happen.  Imap4 had the three branches
            // and this had none (#103).
            std::string phost;
            unsigned int pport = 0;

            const bool proxied = proxy_of(m_url, phost, pport);

            // The constructor already refused a scheme that is neither, so
            // there is no third case to fall through to -- and the one that
            // used to be here tested find("pop"), which is how "pop3s" ended
            // up on a plain socket.
            if(is_secure(m_url)) {
                if(proxied) {
                    sock = new jlib::sys::tlsproxystream(m_url.get_host(),
                                                         m_url.get_port_val(),
                                                         phost, pport);
                }
                else {
                    // The deadline goes in the constructor because the
                    // handshake happens there: pointed at a port that does not
                    // speak TLS, this blocks inside the call and there is no
                    // later moment at which to bound it.
                    sock = new jlib::sys::sslstream(m_url.get_host(),
                                                    m_url.get_port_val(), false,
                                                    -1, m_timeout);
                }
            }
            else if(use_starttls(m_url)) {
                // A tlsstream with delay set connects without handshaking;
                // start() does the handshake in place once STLS has been
                // negotiated.  The primitive smtp::send_tls has used all along.
                if(proxied) {
                    sock = new jlib::sys::tlsproxystream(m_url.get_host(),
                                                         m_url.get_port_val(),
                                                         phost, pport, true);
                }
                else {
                    sock = new jlib::sys::tlsstream(m_url.get_host(),
                                                    m_url.get_port_val(), true,
                                                    -1, m_timeout);
                }
            }
            else {
                if(proxied) {
                    sock = new jlib::sys::proxystream(m_url.get_host(),
                                                      m_url.get_port_val(),
                                                      phost, pport);
                }
                else {
                    sock = new jlib::sys::socketstream(m_url.get_host(), m_url.get_port_val());
                }
            }


            // The proxying paths above do not take it in their constructors
            // yet, so they get it here -- which covers the greeting below but
            // not a handshake inside the constructor. The two direct paths
            // pass it in and do not need this.
            if(m_timeout > 0) sock->set_timeout(m_timeout);

            std::string buf;
            jlib::sys::getline(*sock, buf);
            if(!jlib::util::begins(buf, OK)) {
                throw exception(buf);
            }
            if(use_starttls(m_url)) {
                upgrade(*sock);
            }

            handshake(*sock,"USER "+m_url.get_user(), OK);
            handshake(*sock,"PASS "+m_url.get_pass(), OK);

            return sock;
        }
        
        void Pop3::disconnect(jlib::sys::socketstream& sock) {
            handshake(sock,"QUIT", OK);
        }
        
        void Pop3::remove(jlib::sys::socketstream& sock, unsigned int which) {
            handshake(sock,"DELE "+jlib::util::string_value(which), OK);
        }
        
        std::string Pop3::handshake(jlib::sys::socketstream& sock, const std::string& data, const std::string& ok) {
            if(getenv("JLIB_NET_POP3_DEBUG")) std::cout << data << std::endl;
            sock << data << "\r\n" << std::flush;
            std::string buf;
            jlib::sys::getline(sock, buf);
            if(getenv("JLIB_NET_POP3_DEBUG")) std::cout << buf << std::endl;
            if(!jlib::util::begins(buf,ok)) {
                sock.close();
                throw exception(buf);
            }
            return buf;
        }
        
    }
}
