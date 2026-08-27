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

#include <jlib/net/net.hh>
#include <jlib/net/Imap4.hh>
#include <jlib/net/oauth.hh>

#include <jlib/net/imap_response.hh>

#include <jlib/sys/sys.hh>
#include <jlib/sys/sslstream.hh>
#include <jlib/sys/sslproxystream.hh>

#include <jlib/util/util.hh>
#include <jlib/util/URL.hh>

#include <algorithm>
#include <sstream>

const int PORT = 143;
const int SSL_PORT = 993;
// Both spellings.  "imaps" is what IANA registered and what anyone would
// write; "simap" is the older convention this library was written with and is
// in config files that predate the other.
const std::string SSL_PROTOCOLS[] = { "imaps", "simap" };
const std::string OK = "* OK";
const std::string INTERNAL = "Subject: DON'T DELETE THIS MESSAGE -- FOLDER INTERNAL DATA";
const int TAG_WIDTH = 5;
const std::string ENDL = "\r\n";
const unsigned int MAX_CONNECT_ATTEMPTS = 5;

namespace jlib {
    namespace net {


        ListItem::ListItem() {}
        ListItem::ListItem(const std::string& line) {
            // Eighty lines of find() and tokenize() used to live here, with a
            // comment saying that reading LIST properly needed the grammar
            // rather than more guards.  This is that grammar.
            //
            // What it got wrong, when it got anything wrong, was quiet: a
            // mailbox name in a literal was invisible because the response
            // never arrived whole; a name containing a quote, a space or a
            // parenthesis was cut in the wrong place; and a truncated reply
            // produced a nameless item the caller then skipped.
            imap::response r;

            try {
                // The reader hands back the response without its CRLF, and
                // the grammar wants it.
                r = imap::response::parse(line + "\r\n");
            }
            catch(imap::error& e) {
                if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                    std::cout << "not a LIST response: " << e.what() << std::endl;
                }

                return;
            }

            if(r.name() != "LIST" && r.name() != "LSUB") return;

            m_name = r.mailbox();
            m_delim = r.delimiter();
            m_attr = r.flags();

            m_is_folder = true;
            m_is_parent = true;

            for(const std::string& a : m_attr) {
                const std::string flag = util::upper(a);

                if(flag == "\\NOSELECT")         m_is_folder = false;
                else if(flag == "\\NOINFERIORS") m_is_parent = false;
            }

            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                std::cout << "m_name = <"<<m_name<<">: m_delim = <"<<m_delim<<">: m_attr = ";
                for(const std::string& a : m_attr) std::cout << "<"<<a<<">";
                std::cout << "is_folder(): " << is_folder() << "; "
                          << "is_parent(): " << is_parent() << std::endl;
            }
        }
        
        std::vector<std::string> ListItem::get_attributes() { return m_attr; }
        std::string ListItem::get_delim() { return m_delim; }
        std::string ListItem::get_name() { return m_name; }
        
        bool ListItem::is_folder() { 
            return m_is_folder;
        }

        bool ListItem::is_parent() { 
            return m_is_parent;
        }




        Imap4::Imap4(util::URL url) 
            : m_url(url)
        {
            if(util::lower(url.get_protocol()).find("imap") == std::string::npos) {
                throw exception("bad protocol in URL: "+url());
            }

            m_user = url.get_user();
            m_pass = url.get_pass();
            m_host = url.get_host();

            if(url.get_port() != "") {
                m_port = util::int_value(url.get_port());
            }
            else {
                if(is_secure()) {
                    m_port = SSL_PORT;
                }
                else {
                    m_port = PORT;
                }

            }

            m_num = 0;
            m_width=TAG_WIDTH;
            m_state = UnConnected;
        }
        
        Imap4::~Imap4() {
            
        }
        
        bool Imap4::is_secure() {
            // A whole-scheme comparison, not find().  "imaps" does not contain
            // "simap", so the substring test said an imaps:// URL was not
            // secure -- and the caller then opened a plain socketstream to
            // port 143 and sent LOGIN with the password on it.  The standard
            // scheme downgraded silently to no encryption at all.
            const std::string scheme = util::lower(m_url.get_protocol());

            for(const std::string& s : SSL_PROTOCOLS) {
                if(scheme == s) return true;
            }

            return false;
        }

        bool Imap4::use_starttls() {
            return util::lower(m_url["tls"]) == "starttls";
        }

        const std::vector<std::string>& Imap4::capability(sys::socketstream& sock) {
            m_capabilities.clear();

            for(const imap::response& r : command(sock, "CAPABILITY")) {
                if(r.name() != "CAPABILITY") continue;

                m_capabilities = r.capabilities();
            }

            return m_capabilities;
        }

        bool Imap4::has_capability(const std::string& name) const {
            const std::string want = util::upper(name);

            for(const std::string& c : m_capabilities) {
                if(util::upper(c) == want) return true;
            }

            return false;
        }

        Email Imap4::get(sys::socketstream& sock, 
                         int which, 
                         bool only_headers)
        {
            // A hundred and forty lines used to live here.  They tokenized the
            // response on whitespace, looked for "(FLAGS" and then for
            // "RFC822.SIZE" -- and, failing that, for "(RFC822.SIZE", because
            // whether the parenthesis was attached depended on where the
            // attribute happened to fall.  A FETCH response is not a
            // whitespace-separated list of tokens, and every one of those
            // guesses was a way of pretending it is.
            const std::string s = util::string_value(which + 1);
            const std::string what = only_headers ? "RFC822.HEADER" : "RFC822";

            tag(1);

            const std::string req = tag() + " FETCH " + s + ":" + s
                                  + " (FLAGS RFC822.SIZE " + what + ")";

            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << req << std::endl;

            sock << req << ENDL << std::flush;

            Email ret;
            std::set<Email::flag_type> flags;
            std::vector<std::string> flagv;
            bool got = false;

            for(;;) {
                const std::string raw = imap::read(sock);

                if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << raw;

                const imap::response r = imap::response::parse(raw);

                if(r.type() == imap::response::kind::tagged) {
                    if(!r.ok()) throw exception(r.text());

                    break;
                }

                if(r.name() != "FETCH") continue;

                const std::map<std::string,std::string>& att = r.attributes();
                std::map<std::string,std::string>::const_iterator a;

                if((a = att.find("RFC822.SIZE")) != att.end()) {
                    ret.set_data_size(util::int_value(a->second));
                }

                if((a = att.find(what)) != att.end()) {
                    got = true;

                    // Email::create throws on a message it cannot make sense
                    // of, and a mailbox with one bad message in it should
                    // still open.
                    try {
                        ret.create(a->second);
                    }
                    catch(std::exception& e) {
                        if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                            std::cout << "could not read message " << s << ": "
                                      << e.what() << std::endl;
                        }
                    }
                }

                flagv = r.flags();
            }

            if(!got) {
                throw exception("no " + what + " in the response to " + req);
            }

            for(const std::string& f : flagv) {
                if(f == "\\Seen")          flags.insert(Email::seen_flag);
                else if(f == "\\Answered") flags.insert(Email::answered_flag);
                else if(f == "\\Deleted")  flags.insert(Email::deleted_flag);
            }

            // Fetching the body marks it read, whatever the server said.
            if(!only_headers) flags.insert(Email::seen_flag);

            ret.set_flags(flags);

            return ret;
        }

        std::string Imap4::fetch_attribute(sys::socketstream& sock,
                                           unsigned int which,
                                           const std::string& items,
                                           const std::string& want,
                                           unsigned int& size)
        {
            const std::string s = util::string_value(which + 1);
            const std::string req = tag(1) + " FETCH " + s + ":" + s
                                  + " (" + items + ")";

            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << req << std::endl;

            sock << req << ENDL << std::flush;

            std::string ret;
            bool got = false;

            size = 0;

            for(;;) {
                const std::string raw = imap::read(sock);

                if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << raw;

                const imap::response r = imap::response::parse(raw);

                if(r.type() == imap::response::kind::tagged) {
                    if(!r.ok()) throw exception(r.text());

                    break;
                }

                if(r.name() != "FETCH") continue;

                const std::map<std::string,std::string>& att = r.attributes();
                std::map<std::string,std::string>::const_iterator a;

                if((a = att.find("RFC822.SIZE")) != att.end()) {
                    size = util::int_value(a->second);
                }

                if((a = att.find(want)) != att.end()) {
                    ret = a->second;
                    got = true;
                }
            }

            if(!got) throw exception("no " + want + " in the response to " + req);

            return ret;
        }

        std::string Imap4::retrieve_headers(unsigned int which, 
                                       const std::string& mailbox,
                                       unsigned int& size)
        {
            sys::socketstream* sock;
            sock = connect();
            login(*sock);
            examine(*sock,mailbox);

            std::string ret = retrieve_headers(*sock,which,mailbox,size);

            logout(*sock);
            disconnect(*sock);
            delete sock;

            return ret;
        }
        std::string Imap4::retrieve_headers(sys::socketstream& sock, 
                                       unsigned int which, 
                                       const std::string& mailbox,
                                       unsigned int& size) {
            return fetch_attribute(sock, which, "FLAGS RFC822.SIZE RFC822.HEADER",
                                   "RFC822.HEADER", size);
        }

        std::string Imap4::retrieve(int which, const std::string& mailbox) {
            sys::socketstream* sock;
            sock = connect();
            login(*sock);
            select(*sock,mailbox);

            std::string ret = retrieve(*sock,which,mailbox);

            logout(*sock);
            disconnect(*sock);
            delete sock;

            return ret;
        }

        std::string Imap4::retrieve(sys::socketstream& sock, int which, const std::string& mailbox) 
        {
            unsigned int size = 0;
            const std::string ret =
                fetch_attribute(sock, static_cast<unsigned int>(which),
                                "FLAGS RFC822", "RFC822", size);

            // Fetching the body marks it read; say so explicitly rather than
            // relying on the server having done it.
            const std::pair<unsigned int, unsigned int> p(which + 1, which + 1);
            const std::vector<std::string> f { "\\Seen" };

            store(sock, p, "+FLAGS.SILENT", f);

            return ret;
        }
        
        void Imap4::upgrade(sys::socketstream& sock) {
            // RFC 2595 3 and RFC 3501 6.2.1.
            capability(sock);

            if(!has_capability("STARTTLS")) {
                throw exception("the server does not offer STARTTLS, and "
                                "?tls=starttls asked for it");
            }

            command(sock, "STARTTLS");

            // The handshake, with the same verification an imaps:// connection
            // gets: SSL_VERIFY_PEER, the default trust store, and
            // SSL_set1_host on the *server's* name -- which through a proxy is
            // not the name the socket was opened to.
            if(sys::tlsstream* tls = dynamic_cast<sys::tlsstream*>(&sock)) {
                tls->start();
            }
            else if(sys::tlsproxystream* p = dynamic_cast<sys::tlsproxystream*>(&sock)) {
                p->start();
            }
            else {
                throw exception("STARTTLS on a stream that cannot be upgraded");
            }

            // 6.2.1: "The client MUST discard the cached CAPABILITY
            // information and re-issue the command."  Everything read before
            // the handshake was unauthenticated, so a man in the middle could
            // have removed STARTTLS from that list or added an AUTH mechanism
            // to it.  Re-issuing is what makes the list worth having, and it
            // is load-bearing here: login() refuses to send LOGIN when the
            // list says LOGINDISABLED.
            capability(sock);

            if(has_capability("STARTTLS")) {
                throw exception("the server still offers STARTTLS after "
                                "negotiating it, which RFC 3501 6.2.1 forbids");
            }
        }

        sys::socketstream* Imap4::connect() {
            sys::socketstream* sock = 0;
            if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                std::cout << "begin opening "<<m_host<<" on port "<<m_port<<"... "<<std::endl;
            unsigned int i=0;
            while(i<MAX_CONNECT_ATTEMPTS && sock == 0) {
                try {
                    std::string phost;
                    unsigned int pport = 0;

                    // proxy_of(), not tokenize on ":".  What was here indexed
                    // pvec[1] without checking size(), so "?proxy=host" with
                    // no port read off the end of the vector.
                    const bool proxied = proxy_of(m_url, phost, pport);

                    if(proxied && getenv("JLIB_NET_PROXY_DEBUG"))
                        std::cout << "proxy "<<phost<<" on port "<<pport<<std::endl;

                    if(is_secure()) {
                        if(proxied) {
                            sock = new sys::sslproxystream(m_host,
                                                                 m_port,
                                                                 phost, 
                                                                 pport);
                        } else {
                            sock = new sys::sslstream(m_host,m_port);
                        }
                    }
                    else if(use_starttls()) {
                        // Plaintext for now: a tlsstream with delay set
                        // connects without handshaking, and start() does the
                        // handshake in place once the upgrade is negotiated.
                        // The same primitive smtp::send_tls has used all
                        // along.
                        //
                        // The proxy variant exists now that both sit on one
                        // buffer; it used to throw here for want of a
                        // tlsproxystream.
                        if(proxied) {
                            sock = new sys::tlsproxystream(m_host, m_port,
                                                           phost, pport, true);
                        } else {
                            sock = new sys::tlsstream(m_host, m_port, true);
                        }
                    }
                    else {
                        if(proxied) {
                            sock = new sys::proxystream(m_host,
                                                              m_port,
                                                              phost, 
                                                              pport);
                        } else {
                            sock = new sys::socketstream(m_host,m_port);
                        }
                    }
                }
                catch(std::exception& e) {
                    std::cerr << "error creating socket at net:Imap4::connect()"<<std::endl
                              << e.what() << std::endl;
                    sock = 0;
                }
                i++;
            }

            if(sock == 0) {
                std::ostringstream o;
                o << "error creating socket at net:Imap4::connect(), tried "
                  << MAX_CONNECT_ATTEMPTS << " times";
                throw exception(o.str());
            }

            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << "done opening"<<std::endl;

            sock->exceptions(std::ios_base::failbit | std::ios_base::badbit | std::ios_base::eofbit );
            
            std::string buf;
            sys::getline(*sock, buf);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << "read first line: " << buf << std::endl;
            if(!util::begins(buf, OK)) {
                throw exception("error connecting: expected '"+OK+"', received "+buf);
            }

            m_state = NonAuthenticated;

            if(use_starttls()) {
                try {
                    upgrade(*sock);
                }
                catch(...) {
                    delete sock;
                    throw;
                }
            }

            return sock;
            //handshake(sock"LOGIN "+m_user+" "+m_pass);
        }
        
        void Imap4::disconnect(sys::socketstream& sock) {
            //handshake(sock"CLOSE");
            //handshake(sock"LOGOUT");
            sock.close();
            m_state = UnConnected;
        }
        
        void Imap4::remove(int which, const std::string& mailbox) {
            sys::socketstream* sock;
            sock = connect();
            login(*sock);
            select(*sock,mailbox);

            std::pair<unsigned int, unsigned int> p(which+1,which+1);
            std::vector<std::string> f; f.push_back("\\Deleted");
            store(*sock,p,"+FLAGS.SILENT",f);

            logout(*sock);
            disconnect(*sock);
            delete sock;
        }
        
        /*
        bool Imap4::internal(const std::string& data) {
            return util::contains(data, INTERNAL);
        }
        */
        
        bool Imap4::unseen(sys::socketstream& sock, int i) {
            std::string s = util::valueOf(i);
            std::vector<std::string> buf = handshake(sock,"FETCH "+s+":"+s+" (FLAGS FLAGS)");
            for(unsigned int i=0;i<buf.size();i++) {
                if(util::contains(buf[i], "\\Seen"))
                    return false;
            }
            return true;
        }

        std::vector<std::string> Imap4::handshake(sys::socketstream& sock, const std::string& data) {
            std::string buf;
            std::string com = tag(1)+" "+data;
            std::vector<std::string> ret;
            bool idle = (data == "IDLE");

            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                // Two commands carry a credential in an argument, and the
                // masking here knew about one of them.  AUTHENTICATE's
                // argument is a base64 SASL message: under PLAIN that is the
                // password and under XOAUTH2 it is a bearer token, either of
                // which printed in full to stdout is worse than the password
                // this was written to hide.  Both are cut to their first
                // argument -- the username, or the mechanism name.
                const std::string verb = util::upper(util::tokenize(data).empty()
                                                     ? std::string()
                                                     : util::tokenize(data)[0]);

                if(verb == "LOGIN" || verb == "AUTHENTICATE") {
                    std::vector<std::string> tok = util::tokenize(data);

                    if(tok.size() >= 2) {
                        std::cout << tag()<<" "<<tok[0] << " " << tok[1] << " ********"<<std::endl;
                    }
                    else {
                        std::cout << tag() << " " << verb << " **** ********"<<std::endl;
                    }
                }
                else {
                    std::cout << com << std::endl;
                }
            }
            sock << com << ENDL << std::flush;

            const std::string end = (idle ? "+" : tag());

            // One *response* at a time, not one line.  RFC 3501 4.3 lets a
            // response carry a literal -- "{32}" CRLF and then that many
            // octets of message content -- and those octets may contain a line
            // that looks exactly like the tagged completion this loop is
            // waiting for.  Reading lines therefore stopped in the middle of a
            // message, and every command after it read the rest of that
            // message as its own response.
            while(!util::begins(buf, end)) {
                buf = imap::read(sock);

                // The trailing CRLF comes off, as sys::getline used to take
                // it, so that what a caller sees is unchanged for every
                // response that has no literal in it.
                while(!buf.empty() && (buf.back() == '\n' || buf.back() == '\r')) {
                    buf.pop_back();
                }

                if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << buf << std::endl;
                ret.push_back(buf);
            }
            if(util::ibegins(buf, tag()+" NO") || util::ibegins(buf, tag()+" BAD")) {
                std::string err = buf.substr(tag().length()+1);
                throw exception(err);
            }

            if(idle) {
                if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                    std::cout << "DONE" << std::endl;
                sock << "DONE" << ENDL << std::flush;

                while(!util::begins(buf, tag())) {
                    buf = imap::read(sock);

                    while(!buf.empty() && (buf.back() == '\n' || buf.back() == '\r')) {
                        buf.pop_back();
                    }

                    if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << buf << std::endl;
                    ret.push_back(buf);
                }
                if(util::ibegins(buf, tag()+" NO") || util::ibegins(buf, tag()+" BAD")) {
                    std::string err = buf.substr(tag().length()+1);
                    throw exception(err);
                }
            }

            return ret;
        }


        std::string Imap4::tag(int i) {
            num(num()+i);
            return ("A"+util::valueOf(num(), m_width));
        }

        void Imap4::parse(std::vector<std::string> hand) {
            // icontains(line, "exists") matched any line with those letters
            // anywhere in it, and then read tok[1] with no size check.  The
            // UNSEEN branch read tok[2], which on
            //
            //     * OK [UNSEEN 1] First unseen.
            //
            // is the string "[UNSEEN" -- so int_value gave 0 and the unseen
            // count has been zero for as long as there has been one.
            for(const std::string& line : hand) {
                imap::response r;

                try {
                    r = imap::response::parse(line + "\r\n");
                }
                catch(imap::error&) {
                    continue;
                }

                if(r.name() == "EXISTS")      exists(r.number());
                else if(r.name() == "RECENT") recent(r.number());

                // The unseen count is a response-text-code on an OK, not a
                // data response: "* OK [UNSEEN 12] Message 12 is first unseen".
                if(util::ibegins(r.code(), "UNSEEN ")) {
                    unseen(util::int_value(r.code().substr(7)));
                }
            }
        }



        std::vector<std::string> Imap4::noop(sys::socketstream& sock) {
            std::vector<std::string> ret = handshake(sock,"NOOP");
            parse(ret);
            return ret;
        }

        std::vector<std::string> Imap4::idle(sys::socketstream& sock) {
            std::vector<std::string> ret = handshake(sock,"IDLE");
            parse(ret);
            return ret;
        }

        std::vector<std::string> Imap4::idle_send(sys::socketstream& sock) {
            std::string buf;
            std::string data = "IDLE";
            std::string com = tag(1)+" "+data;
            std::vector<std::string> ret;

            m_idle = true;
            
            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                std::cout << com << std::endl;
            }
            sock << com << ENDL << std::flush;

            std::string end = "+";

            while(!util::begins(buf, end)) {
                sys::getline(sock, buf);
                if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << buf << std::endl;
                ret.push_back(buf);
            }
            if(util::ibegins(buf, tag()+" NO") || util::ibegins(buf, tag()+" BAD")) {
                std::string err = buf.substr(tag().length()+1);
                throw exception(err);
            }

            parse(ret);
            return ret;
        }

        std::vector<std::string> Imap4::idle_done(sys::socketstream& sock) {
            std::string buf;
            std::vector<std::string> ret;

            if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                std::cout << "DONE" << std::endl;
            sock << "DONE" << ENDL << std::flush;

            m_idle = false;
            
            while(!util::begins(buf, tag())) {
                sys::getline(sock, buf);
                if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << buf << std::endl;
                ret.push_back(buf);
            }
            if(util::ibegins(buf, tag()+" NO") || util::ibegins(buf, tag()+" BAD")) {
                std::string err = buf.substr(tag().length()+1);
                throw exception(err);
            }

            parse(ret);
            return ret;
        }

        void Imap4::logout(sys::socketstream& sock) {
            handshake(sock,"LOGOUT");
            m_state = UnConnected;
        }
        void Imap4::authenticate(sys::socketstream& sock, const std::string& name,
                                 const sasl_responder& respond)
        {
            // Not through handshake() or command(): both loop until a tagged
            // response and neither can answer a "+", which is the whole of
            // this exchange.
            const std::string com = tag(1) + " AUTHENTICATE " + name;

            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << com << std::endl;

            sock << com << ENDL << std::flush;

            // A server that keeps issuing challenges and a responder that
            // keeps answering the same thing would spin here forever.  No
            // mechanism jlib speaks needs more than two rounds; the cap is
            // loose enough not to matter and finite, which is the point.
            const int MAX_ROUNDS = 8;
            int rounds = 0;

            std::vector<std::string> capabilities;

            for(;;) {
                const std::string raw = imap::read(sock);

                if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << raw;

                imap::response r;

                try {
                    r = imap::response::parse(raw);
                }
                catch(imap::error& e) {
                    // Unparseable mid-exchange: the connection is no longer at
                    // a known boundary, so cancelling would only add to the
                    // confusion.  Say so and let the caller drop it.
                    throw exception(std::string("AUTHENTICATE: ") + e.what());
                }

                if(r.type() == imap::response::kind::continuation) {
                    std::string answer;

                    if(++rounds > MAX_ROUNDS) {
                        cancel_authenticate(sock);

                        throw exception("AUTHENTICATE " + name + ": the server is still "
                                        "challenging after " + util::valueOf(MAX_ROUNDS) +
                                        " rounds");
                    }

                    try {
                        // 6.2.2: the challenge is base64.  Decoded here so a
                        // responder sees the mechanism's own bytes -- the JSON
                        // error blob XOAUTH2 sends on failure, for one --
                        // rather than having to know the encoding.
                        answer = respond(util::base64::decode(r.text()));
                    }
                    catch(...) {
                        // The responder gave up.  Cancelling is not politeness:
                        // without it the server is still waiting for a line,
                        // and the next command sent on this socket is read as
                        // the answer to this challenge.
                        cancel_authenticate(sock);
                        throw;
                    }

                    sock << util::base64::encode(answer) << ENDL << std::flush;

                    if(getenv("JLIB_NET_IMAP4_DEBUG"))
                        std::cout << "<" << answer.length() << " octets of "
                                  << name << " response elided>" << std::endl;

                    continue;
                }

                if(r.type() == imap::response::kind::untagged) {
                    if(r.name() == "CAPABILITY") capabilities = r.capabilities();

                    continue;
                }

                if(!r.ok()) throw exception(r.text());

                // 6.2.2: the capability list changes on authentication, and a
                // client that goes on using the one it took before is reading
                // stale advertising -- LOGINDISABLED and the AUTH= mechanisms
                // are exactly what drops off it.  A server may hand the new
                // one back in the tagged OK's response code, or as an untagged
                // response during the exchange; failing both, the cache is
                // cleared rather than kept, and capability() will fetch it.
                if(util::ibegins(r.code(), "CAPABILITY ")) {
                    m_capabilities = util::tokenize(r.code().substr(11));
                }
                else {
                    m_capabilities = capabilities;
                }

                m_state = Authenticated;

                return;
            }
        }

        void Imap4::authenticate_xoauth2(sys::socketstream& sock,
                                         const std::string& user,
                                         const std::string& access)
        {
            // A list that has been fetched and does not offer it is an
            // objection; one that has never been fetched is not.  login()
            // makes the same distinction the other way round, refusing only
            // when LOGINDISABLED is positively present.
            if(!m_capabilities.empty() && !has_capability("AUTH=XOAUTH2")) {
                throw exception("the server does not offer AUTH=XOAUTH2");
            }

            if(access.empty()) throw exception("AUTHENTICATE XOAUTH2: no access token");

            int round = 0;

            authenticate(sock, "XOAUTH2",
                         [&round, &user, &access](const std::string&) -> std::string {
                             if(++round == 1) return oauth::xoauth2(user, access);

                             // The second challenge is the failure report --
                             // base64 JSON, "status" and "schemes" -- and the
                             // server is waiting for an empty line before it
                             // will send the tagged NO that says the same thing
                             // in a form this code can throw.
                             return std::string();
                         });
        }

        void Imap4::cancel_authenticate(sys::socketstream& sock) {
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << "*" << std::endl;

            sock << "*" << ENDL << std::flush;

            // Read to the tagged completion -- which is a BAD, since the
            // exchange was cancelled -- so the connection is left at a
            // response boundary and stays usable.  Anything thrown in here is
            // swallowed: this runs while another exception is on its way out,
            // and that one is the interesting one.
            try {
                for(;;) {
                    const imap::response r = imap::response::parse(imap::read(sock));

                    if(r.type() == imap::response::kind::tagged) return;

                    // A second continuation after a cancel would mean the
                    // server did not take it; there is nothing further to try.
                    if(r.type() == imap::response::kind::continuation) return;
                }
            }
            catch(...) {
            }
        }

        void Imap4::authenticate_plain(sys::socketstream& sock,
                                       const std::string& user,
                                       const std::string& pass)
        {
            const std::string u = user.empty() && pass.empty() ? m_user : user;
            const std::string p = user.empty() && pass.empty() ? m_pass : pass;

            // RFC 4616 2: the three fields are separated by NUL, so a NUL in
            // any of them makes the message mean something else.  The RFC
            // forbids it outright; refusing beats sending a credential that
            // has been silently cut in half.
            if(u.find('\0') != std::string::npos || p.find('\0') != std::string::npos) {
                throw exception("AUTHENTICATE PLAIN: a NUL in the username or "
                                "password would change where the fields end");
            }

            authenticate(sock, "PLAIN", [&u, &p](const std::string&) {
                return std::string(1, '\0') + u + std::string(1, '\0') + p;
            });
        }
        void Imap4::login(sys::socketstream& sock, const std::string& user, const std::string& pass) {
            // RFC 3501 6.2.3: a server advertising LOGINDISABLED will refuse
            // LOGIN, and a client that sends it anyway has put the password on
            // the wire for nothing.  The list this consults is the one taken
            // *after* any STARTTLS, which is the whole reason 6.2.1 requires
            // it be re-issued.
            if(has_capability("LOGINDISABLED")) {
                throw exception("the server advertises LOGINDISABLED; refusing "
                                "to send a password it will not accept");
            }

            if(user!="" && pass != "") {
                handshake(sock, "LOGIN " + imap::quote(user) + " " + imap::quote(pass));
            }
            else {
                handshake(sock, "LOGIN " + imap::quote(m_user) + " " + imap::quote(m_pass));
            }
            m_state = Authenticated;
        }

        std::vector<std::string> Imap4::select(sys::socketstream& sock, const std::string& path) {
            std::vector<std::string> config = handshake(sock, "SELECT " + imap::quote(path));
            parse(config);
            m_state = Selected;
            return config;
        }
        
        std::vector<std::string> Imap4::examine(sys::socketstream& sock, const std::string& path) {
            std::vector<std::string> ret = handshake(sock, "EXAMINE " + imap::quote(path));
            parse(ret);
            m_state = Selected;
            return ret;
        }

        void Imap4::create(sys::socketstream& sock, const std::string& path) {
            handshake(sock, "CREATE " + imap::quote(path));
        }
        void Imap4::remove(sys::socketstream& sock, const std::string& path) {
            handshake(sock, "DELETE " + imap::quote(path));
        }
        void Imap4::rename(sys::socketstream& sock, const std::string& old_name, const std::string& new_name) {
            handshake(sock, "RENAME " + imap::quote(old_name) + " " + imap::quote(new_name));
        }
        void Imap4::subscribe(sys::socketstream& sock, const std::string& path) {
            handshake(sock, "SUBSCRIBE " + imap::quote(path));
        }
        void Imap4::unsubscribe(sys::socketstream& sock, const std::string& path) {
            handshake(sock, "UNSUBSCRIBE " + imap::quote(path));
        }

        std::vector<ListItem> Imap4::list(sys::socketstream& sock, const std::string& ref, const std::string& path) {
            std::vector<ListItem> ret;
            std::vector<std::string> ls = handshake(sock, "LIST " + imap::quote(ref) + " " + imap::quote(path));

            // i + 1 < size(), not i < size() - 1: the last response is the
            // tagged completion and is not a list item, and on an empty vector
            // size() - 1 is SIZE_MAX.
            for(std::size_t i = 0; i + 1 < ls.size(); i++) {
                ret.push_back(ListItem(ls[i]));
            }

            return ret;
        }

        std::vector<ListItem> Imap4::lsub(sys::socketstream& sock, const std::string& ref, const std::string& path) {
            std::vector<ListItem> ret;

            // LSUB, not LIST.  This sent LIST, so asking for the subscribed
            // mailboxes returned all of them -- which is not a parse error, or
            // any kind of error, just the wrong answer.
            std::vector<std::string> ls = handshake(sock, "LSUB " + imap::quote(ref) + " " + imap::quote(path));

            for(std::size_t i = 0; i + 1 < ls.size(); i++) {
                ret.push_back(ListItem(ls[i]));
            }

            return ret;
        }

        void Imap4::append(sys::socketstream& sock, const std::string& path, const std::string& data, const std::string& flag, const std::string& date) {
            std::string buf;

            // On a local; this overwrote its own parameter.
            const std::string box =
                (path == "INBOX" ? path : (m_url.get_path_no_slash() + path));
            tag(1);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                std::cout << tag() << " APPEND " << imap::quote(path)
                          << " (" << flag << ") {" << data.length() << "}" << std::endl;
            }
            sock << tag() << " APPEND " << imap::quote(path)
                 << " (" << flag << ") {" << data.length() << "}" << ENDL << std::flush;
            sys::getline(sock,buf);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                std::cout <<buf<<std::endl;
            }
            // The server must answer APPEND with a "+" continuation before we
            // send the message body.  This read "!buf.find(...) == buf.npos",
            // where the ! binds to find() alone: the bool result then widens
            // to 0 or 1 and is compared against npos, so it was always false
            // and the throw never fired.  On an error reply we would go on to
            // send the body, which the server then reads as commands.
            //
            // TODO: figure out why the hell I'm getting an 'm' character
            // before the '+' -- which is why this looks for "+" anywhere in
            // the reply rather than requiring it at position 0.
            if(buf.find("+") == buf.npos) {
                throw exception(buf);
            }
            
            sock << data << ENDL << std::flush;
            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                std::cout << data << std::endl;
            }

            sys::getline(sock,buf);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                std::cout << buf << std::endl;
            }

            if(util::ibegins(buf, tag()+" NO") || util::ibegins(buf, tag()+" BAD")) {
                std::string err = buf.substr(tag().length()+1);
                throw exception(err);
            }
        }

        //6.4.    Client Commands - Selected State
        void Imap4::check(sys::socketstream& sock) {
            handshake(sock,"CHECK");
        }
        void Imap4::close(sys::socketstream& sock) {
            handshake(sock,"CLOSE");
            m_state = Authenticated;
        }
        void Imap4::expunge(sys::socketstream& sock) {
            handshake(sock,"EXPUNGE");
        }

                std::vector<imap::response> Imap4::command(sys::socketstream& sock,
                                                   const std::string& data)
        {
            const std::string com = tag(1) + " " + data;

            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << com << std::endl;

            sock << com << ENDL << std::flush;

            std::vector<imap::response> ret;

            for(;;) {
                const std::string raw = imap::read(sock);

                if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << raw;

                const imap::response r = imap::response::parse(raw);

                if(r.type() == imap::response::kind::tagged) {
                    if(!r.ok()) throw exception(r.text());

                    return ret;
                }

                if(r.type() == imap::response::kind::untagged) ret.push_back(r);
            }
        }

        std::vector<unsigned long> Imap4::search(sys::socketstream& sock,
                                                 const std::string& criteria,
                                                 const std::string& charset)
        {
            std::string cmd = "SEARCH ";

            if(!charset.empty()) cmd += "CHARSET " + imap::quote(charset) + " ";

            std::vector<unsigned long> ret;

            // 6.4.4: the answer comes back as untagged SEARCH responses, and
            // a server is allowed to split them across more than one.
            for(const imap::response& r : command(sock, cmd + criteria)) {
                if(r.name() != "SEARCH") continue;

                ret.insert(ret.end(), r.numbers().begin(), r.numbers().end());
            }

            return ret;
        }

        std::vector<imap::response> Imap4::uid(sys::socketstream& sock,
                                               const std::string& command_name,
                                               const std::string& args)
        {
            return command(sock, "UID " + command_name + " " + args);
        }

        std::string Imap4::fetch_partial(sys::socketstream& sock,
                                         unsigned int which,
                                         const std::string& section,
                                         std::size_t origin,
                                         std::size_t length)
        {
            std::ostringstream cmd;
            const std::string s = util::string_value(which + 1);

            cmd << "FETCH " << s << ":" << s << " (BODY.PEEK[" << section << "]<"
                << origin << "." << length << ">)";

            for(const imap::response& r : command(sock, cmd.str())) {
                if(r.name() != "FETCH") continue;

                // The response echoes the section and the *origin* but not the
                // length -- "BODY[]<0>" for a request of "<0.1024>" -- so the
                // key is not the string that was sent, and it comes back as
                // BODY rather than BODY.PEEK.
                for(const auto& a : r.attributes()) {
                    if(util::ibegins(a.first, "BODY[")) return a.second;
                }
            }

            throw exception("no BODY in the response to " + cmd.str());
        }

        std::vector<std::string> Imap4::fetch(sys::socketstream& sock, std::pair<unsigned int,unsigned int> set, std::vector<std::string> n) {
            std::ostringstream cmd;
            cmd << "FETCH "<<set.first<<":"<<set.second<<" (";
            for(unsigned int i=0;i<n.size();i++) {
                cmd << n[i];
                if(i+1 < n.size())
                    cmd << " ";
            }
            cmd << ")";
            std::string com = cmd.str();
            std::vector<std::string> ret = handshake(sock,com);
            return ret;
        }

                std::vector<std::string> Imap4::store(sys::socketstream& sock, std::pair<unsigned int,unsigned int> set, const std::string& key, std::vector<std::string> val) {
            std::ostringstream cmd;
            cmd << "STORE "<<set.first<<":"<<set.second<<" "<<key<<" (";
            for(unsigned int i=0;i<val.size();i++) {
                cmd << val[i];
                if(i+1 < val.size())
                    cmd << " ";
            }
            cmd << ")";
            std::string com = cmd.str();
            
            std::vector<std::string> ret = handshake(sock,com);
            return ret;
        }

        void Imap4::copy(sys::socketstream& sock, std::pair<unsigned int,unsigned int> set, const std::string& box) {
            std::string path = (box == "INBOX" ? box : (m_url.get_path_no_slash() + box));
            std::ostringstream cmd;
            cmd << "COPY " << set.first << ":" << set.second << " " << imap::quote(path);
            std::vector<std::string> ret = handshake(sock,cmd.str());
        }
        
               
    }
}
