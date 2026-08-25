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
#include <jlib/net/address.hh>

#include <jlib/sys/sys.hh>
#include <jlib/sys/tfstream.hh>
#include <jlib/sys/socketstream.hh>
#include <jlib/sys/sslstream.hh>

#include <jlib/util/util.hh>
#include <jlib/util/Regex.hh>
#include <jlib/util/Date.hh>

#include <mutex>
#include <sstream>
#include <stack>
#include <algorithm>

#include <cctype>

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <paths.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

const std::string SYS_MAIL_DIR = std::string(_PATH_MAILDIR);

namespace jlib {
    namespace net {

        long parse_size = 16384;

        void parse_divide(std::istream& is, std::vector<long>& divide, const std::string& div) {
            if(getenv("JLIB_NET_DEBUG")) {
                std::cerr <<"net::parse_divide(is,divide,\""<<div<<"\"): entering"<<std::endl;
            }
            std::string buf;
            bool newline_tail = true;
            int count=is.tellg();
            std::string::size_type p, q;
            while(!is.eof()) {
                if(getenv("JLIB_NET_DEBUG")) {
                    std::cerr << "\treading "<<parse_size<<" bytes from is... " << std::flush;
                }
                sys::getstring(is, buf, parse_size);
                if(getenv("JLIB_NET_DEBUG")) {
                    std::cerr << "\tread " << buf <<std::endl;
                }
                
                p=0;q=0;
                while( (p=buf.find(div,q)) != buf.npos ) {
                    if(getenv("JLIB_NET_DEBUG")) {
                        std::cerr << "\tfound " << div 
                                  << " at p="<<p<<";count="<<count<<std::endl;
                    }
                    if( (p == 0 && newline_tail) || (p>0 && buf[p-1] == '\n') ) {
                        divide.push_back(count+p);
                    }
                    q = p+div.length();
                }
                
                count += buf.length();

                // The *last* byte of this chunk, so the next one can tell
                // whether a divider at its position 0 was preceded by a
                // newline.  This read length()-2, which is the byte before
                // that, and read out of range entirely whenever a chunk came
                // back shorter than two bytes -- including the zero-length
                // read at end of file.  A "From " landing exactly on a 16K
                // chunk boundary was therefore either missed or accepted when
                // it should not have been, depending on what happened to be
                // in memory.
                newline_tail = (!buf.empty() && buf[buf.length()-1] == '\n');
            }

            if(getenv("JLIB_NET_DEBUG")) {
                std::cerr <<"net::parse_divide(): leaving"<<std::endl;
            }
        }

        // Returns size_type rather than long so that npos survives the round
        // trip.  Callers used to store the result in a u_int, which truncates
        // npos (SIZE_MAX) to 0xFFFFFFFF and so never compares equal to it.
        std::string dot_stuff(std::string data) {
            // RFC 5321 4.5.2.  Every line that begins with "." gets another,
            // including the first, because "the first line of the body" is
            // still a line.
            //
            // This used to do something else entirely: it searched the body
            // for the terminator "\r\n.\r\n" and rewrote it as "\r\n. \r\n",
            // inserting a *space* into the message.  That is transparency
            // backwards -- it silently altered the sender's content, and it
            // did nothing at all for a line like ".signature" that does not
            // happen to be a lone dot, which is the case the mechanism exists
            // for.  Such a line ended the DATA command early and the rest of
            // the message was fed to the server as SMTP commands.
            std::string::size_type p = 0;

            while(p < data.size()) {
                if(data[p] == '.') {
                    data.insert(p, 1, '.');
                    p++;
                }

                const std::string::size_type nl = data.find("\r\n", p);
                if(nl == data.npos) break;

                p = nl + 2;
            }

            return data;
        }

        std::string dot_unstuff(std::string data) {
            std::string::size_type p = 0;

            while(p < data.size()) {
                if(data[p] == '.') {
                    data.erase(p, 1);
                }

                const std::string::size_type nl = data.find("\r\n", p);
                if(nl == data.npos) break;

                p = nl + 2;
            }

            return data;
        }

        bool same_address(const std::string& p_addr1, const std::string& p_addr2) {
            // An address that could not be read is not the same as anything,
            // and that includes another address that could not be read.
            //
            // This used to extract both sides, uppercase them, and compare.
            // extract_address returned "" for anything with no @ in it, so
            // both sides came back empty and equal: same_address("Joe Yandle",
            // "Bob Smith") was true, and so was same_address("", "garbage").
            // A function whose whole job is "is this the same person" answered
            // yes for two different people whenever it failed to read either
            // of them, silently, which in a mail client is a misfiled message
            // or a reply sent to the wrong recipient.
            try {
                const std::string a = mailbox::parse(p_addr1, lenient()).addr().str();
                const std::string b = mailbox::parse(p_addr2, lenient()).addr().str();

                // Case folding across the whole address, local part included.
                // RFC 5321 2.4 reserves the local part's case for the
                // receiving host, but that binds relays; jlib is a user agent,
                // every major provider folds, and a client that thought
                // Joe@x.com and joe@x.com were two people would be wrong about
                // every one of them.
                return util::iequals(a, b);
            }
            catch(address::exception&) {
                return false;
            }
        }

        std::string extract_address(const std::string& p_addr) {
            // The convenient spelling of
            // mailbox::parse(s, lenient()).addr().str(), which is what most
            // callers want and all this ever tried to be.
            //
            // It throws now.  Returning "" to mean "I could not read that" is
            // what let same_address decide two strangers were the same person,
            // and a silent empty string in a To: header is a message that goes
            // nowhere with no indication of why.
            return mailbox::parse(p_addr, lenient()).addr().str();
        }

        void build_mime(std::string& data, net::Email& email, bool is_recurse) {
            //cout << "build_mime()"<<endl;
            std::string content_type = email["CONTENT-TYPE"];
            std::string encoding = email["CONTENT-TRANSFER-ENCODING"];

            if(!is_recurse)
                data.clear();

            /*
            std::string key,val;
            util::Headers& h = email.headers();
            util::Headers::iterator i = h.begin();
            for(;i != h.end(); i++) {
                key = i->first; val = i->second;
                key = util::studly_caps(key);
                data += (key+": "+val+"\n");
            }
            */
            data += email.headers();

            if(!is_recurse && email.headers().get("MIME-Version")=="")
                data += "MIME-Version: 1.0\n";

            if(!util::icontains(content_type,"multipart/mixed")) {
                //cout << "!mulitpart"<<endl;
                //data += ("Content-Type: "+content_type+"\n");
                //if(encoding != "") data += ("Content-Transfer-Encoding: "+encoding+"\n");
                data += "\n";
                
                if(encoding == "" || util::icontains(encoding,"7bit") || util::icontains(encoding,"8bit")) {
                    //cout << "7bit encoding"<<endl;
                    data += email.data();
                    //cout << "data = "<< email.data() << endl;
                }
                else if(util::icontains(encoding, "quoted-printable")) {
                    // This used to append the body unencoded while the header
                    // said quoted-printable, so a message with a high byte in
                    // it was declared to be one thing and sent as another.
                    // qp::encode was an empty stub at the time, which is why.
                    data += util::qp::encode(email.data());
                }
                else if(util::icontains(encoding, "base64")) {
                    // RFC 2045 6.8: base64 in a MIME body is broken into
                    // lines of at most 76 characters.  base64::encode no
                    // longer wraps by default, because the other two callers
                    // -- an SMTP AUTH token and an RFC 2047 encoded word --
                    // are both broken by a line break in the middle.  This one
                    // wants it, so it asks.
                    data += util::base64::encode(email.data(), 76);
                }
                else {
                    throw exception("error in net::build_mime(): unknown content-transfer-encoding '"+encoding+"'");
                }
            }
            else {
                //cout << "mulitpart"<<endl;
                //std::string bound = util::valueOf(rand())+"jlib"+util::valueOf(rand());
                std::string bound = util::slice(content_type,"\"","\"");
                data += "\n";
                data += "This is a multi-part message in MIME format.\n\n";
                
                for(std::string::size_type i = 0; i < email.attach().size(); i++) {
                    data += ("--"+bound+"\n");
                    build_mime(data,email.attach()[i],true);
                    if(data[data.length()-1] != '\n') {
                        data += "\n";
                    }
                    if(data[data.length()-2] != '\n') {
                        data += "\n";
                    }
                }
                /* put closing boundary */
                data += ("--" + bound + "--\n\n");
            }    
        }

        bool is_addr(const std::string& s) {
            // Four decimal octets separated by dots, and nothing else.
            //
            // This used to accept any string of digits and dots, which meant
            // it could only ever return false and never true by mistake -- so
            // "" and "...." were both addresses, the loop having nothing to
            // reject.  get_host calls it to decide whether to attempt a
            // reverse lookup.
            int octets = 0;
            std::string::size_type i = 0;

            while(i < s.length()) {
                std::string::size_type digits = 0;
                int value = 0;

                // isdigit takes an int that must be representable as unsigned
                // char; a plain char is signed here, so a high-bit byte would
                // be undefined behaviour.
                while(i < s.length() &&
                      isdigit(static_cast<unsigned char>(s[i]))) {
                    value = value * 10 + (s[i] - '0');
                    digits++;
                    i++;
                }

                if(digits == 0 || digits > 3 || value > 255) {
                    return false;
                }

                octets++;

                if(i == s.length()) {
                    break;
                }

                if(s[i] != '.') {
                    return false;
                }

                i++;

                // A dot has to be followed by another octet.  Without this,
                // "1.2.3.4." consumed the trailing dot, fell out of the loop
                // with four octets counted, and was accepted.
                if(i == s.length()) {
                    return false;
                }
            }

            return (octets == 4);
        }

        std::pair< std::string, std::vector<std::string> > get_host(const std::string& s) {
            struct hostent *h;
            std::pair< std::string, std::vector<std::string> > ret;

            static std::mutex hostex;
            hostex.lock();
            h = gethostbyname(s.c_str());

            if (h == static_cast<struct hostent*>(0)) {
                hostex.unlock();
                throw exception("error resolving "+s);
            }
                        
            if(is_addr(s)) {
                h = gethostbyaddr(h->h_addr_list[0],h->h_length,h->h_addrtype);
                if (h == static_cast<struct hostent*>(0)) {
                    hostex.unlock();
                    throw exception("error doing reverse lookup on "+s);
                }
            }

            ret.first = h->h_name;

            char** p = h->h_addr_list;
            int i=0;
            while(p[i] != static_cast<char*>(0)) {
                char* b = p[i];
                std::string addr;
                for(std::string::size_type j=0;j<static_cast<std::string::size_type>(h->h_length);j++) {
                    int val = static_cast<unsigned char>(b[j]);
                    addr += util::string_value(val);
                    if(j+1<static_cast<std::string::size_type>(h->h_length))
                        addr += ".";
                }
                ret.second.push_back(addr);
                i++;
            }
            hostex.unlock();
            return ret;
        }


        std::string get_ip_string(long addr) {
            struct in_addr a;
            a.s_addr = addr;
            return inet_ntoa(a);
        }

        long get_ip_val(const std::string& addr) {
            return inet_addr(addr.c_str());
        }

        bool is_reserved(const std::string& ip) {
            long twf_ip   = htonl(0x0a000000);
            long twf_mask = htonl(0xff000000);
            long twy_ip   = htonl(0xac100000);
            long twy_mask = htonl(0xfff00000);
            long sxt_ip   = htonl(0xc0a80000);
            long sxt_mask = htonl(0xffff0000);
            
            long loop_ip   = htonl(0x7f000000);
            long loop_mask = htonl(0xff000000);

            long pass_ip = get_ip_val(ip);
            
            return ((pass_ip & twf_mask) == (twf_ip & twf_mask) ||
                    (pass_ip & twy_mask) == (twy_ip & twy_mask) ||
                    (pass_ip & loop_mask) == (loop_ip & loop_mask) ||
                    (pass_ip & sxt_mask) == (sxt_ip & sxt_mask) );
        }


        std::string pathstr(std::list<std::string> path, 
                            const std::string& delim, 
                            bool begin_delim,
                            bool end_delim,
                            bool only_delim) {
            std::string ret;

            std::list<std::string>::iterator i = path.begin();
            std::list<std::string>::iterator j;
            for(;i != path.end();i++) {
                j = i; j++;

                if(begin_delim && i == path.begin()) {
                    ret += delim;
                }

                ret += *i;
                if(j != path.end()) {
                    ret += delim;
                }
                
                if(end_delim && j == path.end()) {
                    ret += delim;
                }
            }
            
            if(only_delim && path.size() == 0) {
                ret += delim;
            }

            return ret;
        }

        namespace mbox {
            std::string make_path(const std::string& maildir, std::list<std::string> path) {
                if(path.size() == 1 && path.front() == "INBOX") {
                    return (std::string(SYS_MAIL_DIR) + "/" + getenv("USER"));
                } else {
                    return (maildir + pathstr(path));
                }

            }

            void create(const std::string& maildir, std::list<std::string> path) {
                create(make_path(maildir, path));
            }

            void create(const std::string& path) {
                std::fstream box(path.c_str(), (std::ios_base::out|std::ios_base::app));
                box.close();
            }

            void deleet(const std::string& maildir, std::list<std::string> path) {
                deleet(make_path(maildir, path));
            }

            void deleet(const std::string& path) {
                int e = unlink(path.c_str());
                if(e == -1) {
                    std::ostringstream o;
                    o << "unable to delete " << path << ": " << strerror(errno);
                    throw exception(o.str());
                }
            }

            void rename(const std::string& maildir, std::list<std::string> src, std::list<std::string> dst) {
                rename(make_path(maildir, src), make_path(maildir, dst));
            }

            void rename(const std::string& src, const std::string& dst) {
                int e = ::rename(src.c_str(), dst.c_str());
                if(e == -1) {
                    std::ostringstream o;
                    o << "unable to rename " << src << " to " << dst << ": " << strerror(errno);
                    throw exception(o.str());
                }
            }

            void remove(const std::string& maildir, std::list<std::string> path, std::list<int> which, std::vector<long> divide) {
                remove(make_path(maildir, path), which, divide);
            }

            void remove(const std::string& path, std::list<int> which, std::vector<long> divide) {
                std::vector<int> phys;
                std::vector<int>::iterator pi;
                for(std::list<int>::iterator i = which.begin(); i != which.end(); i++) {
                    phys.push_back(*i);
                }
                
                std::sort(phys.begin(), phys.end());
                
                if(getenv("JLIB_NET_MBOX_DEBUG")) {
                    for(pi = phys.begin(); pi != phys.end(); pi++) {
                        std::cout << "phys[i] = " << *pi << std::endl;
                    }
                }
                
                std::vector<long> pts;
                for(pi = phys.begin(); pi != phys.end(); pi++) {
                    pts.push_back(divide[*pi]);
                    if((*pi + 1) != divide.size()) {
                        pts.push_back(divide[*pi + 1]);
                    }
                }
                
                if(getenv("JLIB_NET_MBOX_DEBUG")) {
                    for(unsigned int i=0;i<pts.size();i++) {
                        std::cout << "pts[i] = " << pts[i] << std::endl;
                    }
                }
                
                util::file::kill(path, pts);
                
            }
            
            Email get(const std::string& maildir, std::list<std::string> path, int i, std::vector<long> divide, bool oheader) {
                return get(make_path(maildir, path), i, divide, oheader);
            }

            Email get(const std::string& path, int i, std::vector<long> divide, bool oheader) {
                std::ifstream ifs(path.c_str(), std::ios_base::in);
                return get(ifs, i, divide, oheader);
            }


            Email get(std::istream& is, int i, std::vector<long> divide, bool oheader) {
                is.seekg(divide[i], std::ios_base::beg);
                std::string buf;

                if(getenv("JLIB_NET_MBOX_DEBUG")) {
                    std::cout << "net::mbox::get(is, " << i << ", divide, "<< oheader << ")" << std::endl;
                }

                if(getenv("JLIB_NET_MBOX_DEBUG")) {
                    std::cout << "net::mbox::get: is.tellg() " << is.tellg() << std::endl;
                }
                // get rid of "From " pseudoheader
                //sys::getline(is,buf);
                
                if(i+1 == divide.size()) {
                    if(getenv("JLIB_NET_MBOX_DEBUG")) {
                        std::cout << "net::mbox::get: reading to end" << std::endl;
                    }
                    sys::read(is, buf);
                }
                else {
                    if(getenv("JLIB_NET_MBOX_DEBUG")) {
                        std::cout << "net::mbox::get: reading " << (divide[i+1] - divide[i]) << " bytes" << std::endl;
                    }
                    sys::read(is, buf, (divide[i+1] - divide[i]) );
                }

                if(getenv("JLIB_NET_MBOX_DEBUG")) {
                    std::cout << "net::mbox::get: creating buffer from:" << std::endl
                              << buf << std::endl;
                }

                Email ret(buf);
                ret.set_data_size(buf.length());

                if(getenv("JLIB_NET_MBOX_DEBUG")) {
                    std::cout << "net::mbox::get: buffer created:" << std::endl
                              << ret.raw() << std::endl;
                }


                return ret;
             }


            void append(const std::string& maildir, std::list<std::string> path, Email e) {
                append(make_path(maildir, path), e);
            }

            void append(const std::string& path, Email e) {
                std::ofstream ofs(path.c_str(), std::ios_base::out | std::ios_base::app);
                std::ifstream ifs(path.c_str());
                ifs.seekg(-1,std::ios_base::end);
                char c;
                ifs >> c;
                
                if(c != '\n') 
                    ofs << '\n';
                if(e.raw().find("From ") != 0) {
                    util::Date date;
                    ofs << "From MAILER-DAEMON " << util::Date() << "\n";
                }
                ofs << e.raw();
            }
        }

        

        namespace smtp {

            std::vector<std::string> parse(const std::string& field) {
                // RFC 5322's address-list, rather than the excise-then-split
                // this used to be.  That version cut everything between the
                // first quote and the last, so two quoted display names in one
                // header took the text between them with them, and it split on
                // every comma including the ones inside a display name -- so
                // "Yandle, Joseph" <joey@x.com> became two recipients, one of
                // them undeliverable.
                std::vector<std::string> ret;

                for(const mailbox& m : mailbox::parse_list(field, lenient())) {
                    ret.push_back(m.addr().str());
                }

                return ret;
            }


            void handshake(sys::socketstream& stream, const std::string& data, const std::string& ok) {
                if(getenv("JLIB_NET_DEBUG"))
                    std::cerr << "SMTP << "<<data<<std::endl;
                stream << data << "\r\n" << std::flush;
                std::string buf;
                sys::getline(stream, buf);
                
                if(getenv("JLIB_NET_DEBUG"))
                    std::cerr << "SMTP >> "<<buf<<std::endl;

                if(buf.find(ok) != 0) {
                    stream.close();
                    throw exception(buf);
                }
            }
            
            //void send(const std::string& mail, const std::string& rcpt, const std::string& data, 
            //std::string host, const unsigned int port) {

            /** By value deliberately: data is modified in place and returned. */
            std::string convert_to_crlf(std::string data) {
                std::string::size_type p = 0, i;
                while( (i=data.find("\n",p)) != std::string::npos ) {
                    if(i == 0 || data[i-1] != '\r') {
                        data.insert(i,1,'\r');
                        i++;
                    }
                    p = i+1;
                }
                return data;
            }

            void send(const std::string& mail, const std::string& rcpt, const std::string& data, sys::socketstream& stream);
            void finish(const std::string& mail, const std::string& rcpt, const std::string& data, sys::socketstream& stream);

            void send(const std::string& mail, const std::string& rcpt, const std::string& data, const std::string& host, unsigned int port) {
                sys::socketstream stream(host, port);
                send(mail, rcpt, data, stream);
            }

            void send_ssl(const std::string& mail, const std::string& rcpt, const std::string& data, const std::string& host, unsigned int port) {
                sys::sslstream stream(host, port);
                send(mail, rcpt, data, stream);
            }

            std::list<std::string> eshake(sys::socketstream& sock, const std::string& data, const std::string& ok) {
                std::list<std::string> ret;
                std::string buf;
                
                sock << data << "\r\n" << std::flush;
                if(getenv("JLIB_NET_DEBUG"))
                    std::cout << data << std::endl;

                while(buf.find(ok + " ") == std::string::npos) {
                    sys::getline(sock, buf);
                    if(getenv("JLIB_NET_DEBUG"))
                        std::cout << buf << std::endl;
                    ret.push_back(buf.substr(ok.size() + 1));
                }

                return ret;
            }

            void send_tls(const std::string& mail, const std::string& rcpt, const std::string& data, const std::string& host, unsigned int port) {
                sys::tlsstream stream(host, port, true);
                std::list<std::string> r;

                r = eshake(stream, "EHLO localhost", "250");
                if(std::find(r.begin(), r.end(), "STARTTLS") == r.end()) 
                    throw exception("No STARTTLS option");

                handshake(stream, "STARTTLS", "220");
                stream.start();

                r = eshake(stream, "EHLO localhost", "250");

                finish(mail, rcpt, data, stream);
            }
                
            void send_tls_auth(const std::string& mail, const std::string& rcpt, const std::string& data, const std::string& host, unsigned int port, const std::string& user, const std::string& pass) {
                sys::tlsstream stream(host, port, true);
                std::list<std::string> r;

                r = eshake(stream, "EHLO localhost", "250");
                if(std::find(r.begin(), r.end(), "STARTTLS") == r.end()) 
                    throw exception("No STARTTLS option");

                handshake(stream, "STARTTLS", "220");
                stream.start();

                r = eshake(stream, "EHLO localhost", "250");
                bool plain = false;
                for(std::list<std::string>::iterator i = r.begin(); i != r.end(); i++) {
                    if(i->find("AUTH") == 0) {
                        if(i->find("PLAIN") != std::string::npos) {
                            plain = true;
                            break;
                        } else {
                            throw exception("AUTH option does not include plain: " + (*i));
                        }
                    }
                }

                if(!plain)
                    throw exception("No AUTH option");
                
                std::string token = util::base64::encode(std::string(1, '\0') + user + std::string(1, '\0') + pass);
                handshake(stream, "AUTH PLAIN " + token, "235");

                finish(mail, rcpt, data, stream);
            }
                
            void send_ssl_auth(const std::string& mail, const std::string& rcpt, const std::string& data, const std::string& host, unsigned int port, const std::string& user, const std::string& pass) {
                sys::sslstream stream(host, port);
                std::list<std::string> r;

                r = eshake(stream, "EHLO localhost", "250");
                bool plain = false;
                for(std::list<std::string>::iterator i = r.begin(); i != r.end(); i++) {
                    if(i->find("AUTH") == 0) {
                        if(i->find("PLAIN") != std::string::npos) {
                            plain = true;
                            break;
                        } else {
                            throw exception("AUTH option does not include plain: " + (*i));
                        }
                    }
                }

                if(!plain)
                    throw exception("No AUTH option");
                
                std::string token = util::base64::encode(std::string(1, '\0') + user + std::string(1, '\0') + pass);
                handshake(stream, "AUTH PLAIN " + token, "235");

                finish(mail, rcpt, data, stream);
            }
                
            void send(const std::string& mail, const std::string& rcpt, const std::string& data, sys::socketstream& stream) {
                std::string helo = "localhost";
                std::string greet;

                sys::getline(stream, greet);
                if(greet.find("220") != 0) {
                    throw exception(greet);
                }
                
                handshake(stream, "HELO "+helo, "250");

                finish(mail, rcpt, data, stream);
            }

            void finish(const std::string& mail, const std::string& rcpt, const std::string& data, sys::socketstream& stream) {
                handshake(stream, "MAIL FROM: <"+extract_address(mail)+">", "250");
                
                std::vector<std::string> rcptVec = parse(rcpt);
                for(std::vector<std::string>::iterator i=rcptVec.begin(); i != rcptVec.end(); i++) {
                    handshake(stream, "RCPT TO: <"+(*i)+">", "250");
                }
               
                // On a local: this rewrote its own parameter, first to
                // canonical line endings and then to make the body safe to
                // send inside a dot-terminated stream.
                std::string body = dot_stuff(convert_to_crlf(data));

                // The terminator is CRLF "." CRLF, and the leading CRLF is the
                // one that ends the last line of the body -- so a body that
                // already ends in CRLF must not get another, or the message
                // gains a blank line every time it is sent.
                if(body.size() < 2 || body.compare(body.size() - 2, 2, "\r\n") != 0) {
                    body += "\r\n";
                }

                handshake(stream, "DATA", "354");
                handshake(stream, body + ".\r\n", "250");
                
                stream.close();
            }

            /*
            void send(const std::string& mail, const std::string& rcpt, const std::string& data, const std::string& host) {
                send(mail,rcpt,data,host,25);
            }
            void send(const std::string& mail, const std::string& rcpt, const std::string& data) {
                send(mail,rcpt,data,"localhost",25);
            }
            */
            
        }


        namespace http {

            std::string get(util::URL url) {
                std::string endl = "\r\n";
                std::string dendl = endl+endl;
                std::string buf;
                std::string ret;
                std::ostringstream o;
                unsigned int port = 80;

                if(url.get_protocol() != "http") {
                    throw net::exception("error in net::http::get(): bad protocol: "+
                                               url.get_protocol());
                }

                if(url.get_port() != "") {
                    port = url.get_port_val();
                }

                o << "GET "<<url.get_path()<<" HTTP/1.0"<<endl
                  << "Host: "<<url.get_host()<<endl
                  << endl;

                sys::socketstream sock(url.get_host(), port);
                sock << o.str() << std::flush;
                
                std::getline(sock,buf);

                std::vector<std::string> response = util::tokenize(buf);
                if(response.size() < 3) {
                    throw net::exception("error in net::http::get(): bad response: "+
                                               buf);
                }
                
                if(response[1] != "200") {
                    std::ostringstream err;
                    err << "error in net::http::get(): error code "<<response[1]<<": ";
                    for(unsigned int j=2;j<response.size();j++)
                        err << response[j]<< " ";
                        
                    throw net::exception(err.str());
                }

                sys::getstring(sock,buf);
                std::string::size_type p = buf.find(dendl);
                ret = buf.substr(p+dendl.length());

                return ret;
            }

        }

        namespace html {
            std::string render(const std::string& s) {
                sys::tfstream buf;
                std::string out, err;

                buf << s;
                buf.close();

                // The path is jlib's own temporary name, so this was not
                // exploitable -- but it was the same shape as the four that
                // were, and there is no reason for a shell to be here.
                sys::run({ "lynx", "-dump", "-force_html", buf.get_path() },
                         out, err);

                return out;
            }
        }


    }
}

