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

#include <jlib/net/Pop3.hh>
#include <jlib/net/net.hh>

#include <jlib/sys/sys.hh>
#include <jlib/sys/sslstream.hh>

#include <jlib/util/util.hh>

#include <memory>

const int PORT = 110;
const int SPORT = 995;
const std::string OK = "+OK";

namespace jlib {
    namespace net {
        
        Pop3::Pop3(jlib::util::URL url, bool remove) 
            : m_remove(remove)
        {
            m_url = url;
            if(m_url.get_port() == "") {
                if(jlib::util::lower(m_url.get_protocol()).find("spop") != std::string::npos) {
                    m_url.set_port(jlib::util::string_value(SPORT));
                }
                else if(jlib::util::lower(m_url.get_protocol()).find("pop") != std::string::npos) {
                    m_url.set_port(jlib::util::string_value(PORT));
                }
                else {
                    throw exception("bad protocol in jlib::net::Pop3::Pop3(), m_url = "+m_url());
                }
            }
        }
        
        
        std::list<std::string> Pop3::retrieve() {
            std::list<std::string> buf;
            std::unique_ptr<jlib::sys::socketstream> sock(connect());

            //std::string buf = retrieve(sock,which);

            disconnect(*sock);
            return buf;
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
            if(jlib::util::lower(m_url.get_protocol()).find("spop") != std::string::npos) {
                sock = new jlib::sys::sslstream(m_url.get_host(), m_url.get_port_val());
            }
            else if(jlib::util::lower(m_url.get_protocol()).find("pop") != std::string::npos) {
                sock = new jlib::sys::socketstream(m_url.get_host(), m_url.get_port_val());
            }
            else {
                throw exception("bad protocol in jlib::net::Pop3::connect(), m_url = "+m_url());
            }


            std::string buf;
            jlib::sys::getline(*sock, buf);
            if(!jlib::util::begins(buf, OK)) {
                throw exception(buf);
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
