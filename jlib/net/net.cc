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

#include <jlib/crypt/crypt.hh>

#include <jlib/net/net.hh>

#include <jlib/sys/sys.hh>
#include <jlib/sys/tfstream.hh>
#include <glibmm/thread.h>
#include <jlib/sys/socketstream.hh>
#include <jlib/sys/sslstream.hh>

#include <jlib/util/util.hh>
#include <jlib/util/Regex.hh>
#include <jlib/util/Date.hh>

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

        void parse_headers(std::istream& is, std::map<std::string,std::string>& m, bool uppercase) {
            std::string buf, key, val;
            bool bunny = true;
            while(bunny && !is.eof()) {
                sys::getline(is,buf);
                if(buf == "") {
                    bunny = false;
                }
                else {
                    if(isspace(buf[0])) {
                        if(key != "" && val != "") {
                            m[key] = m[key]+" "+util::trim(buf);
                        }
                    }
                    else {
                        /* do decoding of previous header */
                        if(key != "") {
                            std::string buffer = m[key];
                            if(buffer.find("=") != std::string::npos) {
                                static util::Regex reg("(.*)=\\?(.+)\\?([QqBb])\\?(.+)\\?=(.*)");
                                util::Regex::Match match(reg.match(buffer));
                                if(match.size() > 0) {
                                    std::string enc = match[4];
                                    std::string dec;
                                    if(util::upper(match[3]) == "B") {
                                        dec = crypt::base64::decode(enc);
                                    }
                                    else if(util::upper(match[3]) == "Q") {
                                        dec = crypt::qp::decode(enc);
                                    }

                                    m[key] = match[1]+dec+match[5];
                                }
                            }
                        }

                        std::string::size_type j;
                        if( (j=buf.find(":")) != buf.npos ) {
                            key = buf.substr(0,j);
                            if(uppercase)
                                key = util::upper(key);
                            val = buf.substr(j+1);
                            if(key.find("FROM ") == 0) {
                                key = "";
                            }
                            else {
                                //cerr << "setting m[\""<<key<<"\"] = "<<val<<endl;
                                m[key] = util::trim(val);
                            }
                        }
                    }
                }
            }
        }

        std::vector<std::string> parse_header(std::istream& stream, std::string header) {
            std::vector<std::string> ret;
            std::string buf, key, val;
            std::vector<std::string>::iterator current = ret.end();
            bool bunny = true;
            while(bunny && !stream.eof()) {
                sys::getline(stream,buf);
                if(buf == "") {
                    bunny = false;
                }
                else {
                    /* wrapped header */
                    if(isspace(buf[0])) {
                        if(current != ret.end()) {
                            val += util::trim(buf);
                            *current = val;
                        }
                    }
                    else {
                        /* do decoding of previous header */
                        if(current != ret.end()) {
                            std::string buffer = *current;
                            if(buffer.find("=") != std::string::npos) {
                                static util::Regex reg("(.*)=\\?(.+)\\?([QqBb])\\?(.+)\\?=(.*)");
                                util::Regex::Match match(reg.match(buffer));
                                if(match.size() > 0) {
                                    std::string enc = match[4];
                                    std::string dec;
                                    if(util::upper(match[3]) == "B") {
                                        dec = crypt::base64::decode(enc);
                                    }
                                    else if(util::upper(match[3]) == "Q") {
                                        dec = crypt::qp::decode(enc);
                                    }

                                    *current = match[1]+dec+match[5];
                                }
                            }
                        }

                        key = "";
                        val = "";
                        current = ret.end();
                        std::string::size_type j;
                        if( (j=buf.find(":")) != buf.npos ) {
                            key = buf.substr(0,j);
                            if(util::upper(key) == util::upper(header)) {
                                val = util::trim(buf.substr(j+1));
                                ret.push_back(val);
                                current = (ret.end()-1);
                            }
                            else {
                                key = "";
                                val = "";
                            }
                        }
                    }
                }
            }

            return ret;
        }
        
        void parse_divide(std::istream& is, std::vector<long>& divide, std::string div) {
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
                newline_tail = (buf[buf.length()-2] == '\n');
            }

            if(getenv("JLIB_NET_DEBUG")) {
                std::cerr <<"net::parse_divide(): leaving"<<std::endl;
            }
        }

        long find_end(std::string s, const std::vector<std::string>& e) {
            long ret = s.npos;
            std::string::size_type p;
            for(std::string::size_type i=0;i<e.size();i++) {
                if( (p=s.find(e[i])) != s.npos ) {
                    if(ret == s.npos) {
                        ret = p;
                    }
                    else {
                        if(p < ret)
                            ret = p;
                    }
                }
            }
            return ret;
        }

        std::string parse_end(std::string s, const std::vector<std::string>& ends) {
            if(ends.size() == 0) {
                return s;
            }
            
            u_int p = find_end(s,ends);
            if(p != s.npos) {
                return s.substr(0,p);
            }
            else {
                return s;
            }
            
        }

        void parse_end(std::istream& is, std::vector<std::string> ends, std::string& raw) {
            // if we don't have any ends, just grab it all
            if(ends.size() == 0) {
                sys::getstring(is,raw);
                return;
            }
            long beg = is.tellg();
            std::string buf;
            std::string tmp;
            int count=0;
            std::string::size_type p, q;
            while(!is.eof()) {
                sys::getstring(is, buf, parse_size);
                
                p=0;q=0;
                if( (p=find_end(buf,ends)) != buf.npos  ) {
                    tmp += buf.substr(0,p);
                    is.seekg(beg+count+p, std::ios_base::beg);
                    raw = tmp;
                    return;
                }
                else {
                    tmp += buf;
                    count += buf.length();
                }
            }
            raw = tmp;
        }

        bool same_address(std::string p_addr1, std::string p_addr2) {
            std::string addr1, addr2;
            
            try {
                addr1 = util::upper(extract_address(p_addr1));
                addr2 = util::upper(extract_address(p_addr2));
            }
            catch(exception& e) {
                return false;
            }
            
            return (addr1 == addr2);
        }
        
        std::string extract_address(std::string p_addr) {
            u_int p = p_addr.find("@");
            int m=0,n=-1;
                
            if(p == p_addr.npos) {
                return std::string();
            }
            else {
                for(int i=p+1;i<(int)p_addr.length();i++) {
                    if(!Email::is_valid(p_addr[i])) {
                        n = i;
                        break;
                    }
                }
                for(int i=p-1;i>=0;i--) {
                    if(!Email::is_valid(p_addr[i])) {
                        m = i+1;
                        break;
                    }
                }
                
                return p_addr.substr(m,(n-m));
            }
        }

        std::list<std::string> extract_addresses(std::string s) {
            std::list<std::string> tokens = util::tokenize_list(s, ",");
            std::list<std::string> ret;

            for(std::list<std::string>::iterator i = tokens.begin(); i != tokens.end(); i++) {
                std::string ex = extract_address(*i);
                if(ex != "") {
                    ret.push_back(ex);
                }
            }
            
            return ret;
        }

        std::list<std::string> split_addresses(std::string s) {
            std::list<std::string> ret;
            std::string token;
            u_int x = 0, y;
            bool p = false, q = false;
            std::stack<std::string> nest;
            for(u_int i = 0; i < s.length(); i++) {
                char c = s[i];

                // if we're in an nested parse, look for the end
                if(nest.size()) {
                    if(nest.top() == "\"" && c == '"') {
                        nest.pop();
                        continue;
                    }
                    if(nest.top() == "(" && c == ')') {
                        nest.pop();
                        continue;
                    }
                }
                if(c == '(') {
                    nest.push("(");
                    continue;
                }
                if(c == '"') {
                    nest.push("\"");
                    continue;
                }

                if(nest.size() == 0) {
                    if(c == ',') {
                        if(i > x) {
                            std::string t = util::trim(s.substr(x, i-x));
                            if(t != "" && t.find("@") != std::string::npos) {
                                ret.push_back(t);
                            }
                        }
                        x = i+1;
                        continue;
                    }
                }
            }

            if(x < (s.length()-1)) {
                std::string t = util::trim(s.substr(x));
                if(t != "" && t.find("@") != std::string::npos) {
                    ret.push_back(t);
                }
            }

            return ret;
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
                    //cout << "QP encoding"<<endl;
                    data += email.data();
                    //cout << "data = "<< email.data() << endl;
                }
                else if(util::icontains(encoding, "base64")) {
                    //cout << "base64 encoding"<<endl;
                    data += crypt::base64::encode(email.data());
                    //cout << "data = "<< email.data() << endl;
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

        bool is_addr(std::string s) {
            for(std::string::size_type i=0;i<s.length();i++) {
                if(!isdigit(s[i]) && s[i] != '.')
                    return false;
            }
            return true;
        }

        std::pair< std::string, std::vector<std::string> > get_host(std::string s) {
            struct hostent *h;
            std::pair< std::string, std::vector<std::string> > ret;

            static Glib::Mutex hostex;
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

        long get_ip_val(std::string addr) {
            return inet_addr(addr.c_str());
        }

        bool is_reserved(std::string ip) {
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
                            std::string delim, 
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
            std::string make_path(std::string maildir, std::list<std::string> path) {
                if(path.size() == 1 && path.front() == "INBOX") {
                    return (std::string(SYS_MAIL_DIR) + "/" + getenv("USER"));
                } else {
                    return (maildir + pathstr(path));
                }

            }

            void create(std::string maildir, std::list<std::string> path) {
                create(make_path(maildir, path));
            }

            void create(std::string path) {
                std::fstream box(path.c_str(), (std::ios_base::out|std::ios_base::app));
                box.close();
            }

            void deleet(std::string maildir, std::list<std::string> path) {
                deleet(make_path(maildir, path));
            }

            void deleet(std::string path) {
                int e = unlink(path.c_str());
                if(e == -1) {
                    std::ostringstream o;
                    o << "unable to delete " << path << ": " << strerror(errno);
                    throw exception(o.str());
                }
            }

            void rename(std::string maildir, std::list<std::string> src, std::list<std::string> dst) {
                rename(make_path(maildir, src), make_path(maildir, dst));
            }

            void rename(std::string src, std::string dst) {
                int e = ::rename(src.c_str(), dst.c_str());
                if(e == -1) {
                    std::ostringstream o;
                    o << "unable to rename " << src << " to " << dst << ": " << strerror(errno);
                    throw exception(o.str());
                }
            }

            void remove(std::string maildir, std::list<std::string> path, std::list<int> which, std::vector<long> divide) {
                remove(make_path(maildir, path), which, divide);
            }

            void remove(std::string path, std::list<int> which, std::vector<long> divide) {
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
            
            Email get(std::string maildir, std::list<std::string> path, int i, std::vector<long> divide, bool oheader) {
                return get(make_path(maildir, path), i, divide, oheader);
            }

            Email get(std::string path, int i, std::vector<long> divide, bool oheader) {
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


            void append(std::string maildir, std::list<std::string> path, Email e) {
                append(make_path(maildir, path), e);
            }

            void append(std::string path, Email e) {
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

            std::vector<std::string> parse(std::string field) {
                std::vector<std::string> ret;
                std::string tmp = field;
                tmp = util::excise(tmp, "\"", "\"");
                tmp = util::excise(tmp, "(", ")");
                
                ret = util::tokenize(tmp, ",");
                for(std::vector<std::string>::iterator i = ret.begin(); i != ret.end(); i++) {
                    *i = extract_address(*i);
                }
                
                return ret;
            }


            void handshake(sys::socketstream& stream, std::string data, std::string ok) {
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
            
            //void send(std::string mail, std::string rcpt, std::string data, 
            //std::string host, const unsigned int port) throw(exception) {

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

            void send(std::string mail, std::string rcpt, std::string data, sys::socketstream& stream);
            void finish(std::string mail, std::string rcpt, std::string data, sys::socketstream& stream);

            void send(std::string mail, std::string rcpt, std::string data, std::string host, unsigned int port) {
                sys::socketstream stream(host, port);
                send(mail, rcpt, data, stream);
            }

            void send_ssl(std::string mail, std::string rcpt, std::string data, std::string host, unsigned int port) {
                sys::sslstream stream(host, port);
                send(mail, rcpt, data, stream);
            }

            std::list<std::string> eshake(sys::socketstream& sock, std::string data, std::string ok) {
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

            void send_tls(std::string mail, std::string rcpt, std::string data, std::string host, unsigned int port) {
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
                
            void send_tls_auth(std::string mail, std::string rcpt, std::string data, std::string host, unsigned int port, std::string user, std::string pass) {
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
                
                std::string token = crypt::base64::encode(std::string(1, '\0') + user + std::string(1, '\0') + pass);
                handshake(stream, "AUTH PLAIN " + token, "235");

                finish(mail, rcpt, data, stream);
            }
                
            void send_ssl_auth(std::string mail, std::string rcpt, std::string data, std::string host, unsigned int port, std::string user, std::string pass) {
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
                
                std::string token = crypt::base64::encode(std::string(1, '\0') + user + std::string(1, '\0') + pass);
                handshake(stream, "AUTH PLAIN " + token, "235");

                finish(mail, rcpt, data, stream);
            }
                
            void send(std::string mail, std::string rcpt, std::string data, sys::socketstream& stream) {
                std::string helo = "localhost";
                std::string greet;

                sys::getline(stream, greet);
                if(greet.find("220") != 0) {
                    throw exception(greet);
                }
                
                handshake(stream, "HELO "+helo, "250");

                finish(mail, rcpt, data, stream);
            }

            void finish(std::string mail, std::string rcpt, std::string data, sys::socketstream& stream) {
                handshake(stream, "MAIL FROM: <"+extract_address(mail)+">", "250");
                
                std::vector<std::string> rcptVec = parse(rcpt);
                for(std::vector<std::string>::iterator i=rcptVec.begin(); i != rcptVec.end(); i++) {
                    handshake(stream, "RCPT TO: <"+(*i)+">", "250");
                }
               
                data = convert_to_crlf(data);
                std::string eom = "\r\n.\r\n";
                std::string neom = "\r\n. \r\n";
                while(data.find(eom) != data.npos) {
                    data.replace(data.find(eom), eom.length(), neom);
                }
                
                handshake(stream, "DATA", "354");
                handshake(stream, data+eom, "250");
                
                stream.close();
            }

            /*
            void send(std::string mail, std::string rcpt, std::string data, std::string host) throw(exception) {
                send(mail,rcpt,data,host,25);
            }
            void send(std::string mail, std::string rcpt, std::string data) throw(exception) {
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
            std::string render(std::string s) {
                sys::tfstream buf;
                std::string cmd,out,err;

                buf << s;
                buf.close();
                cmd = "lynx -dump -force_html "+buf.get_path();
                sys::shell(cmd,out,err);
                    
                return out;
            }
        }


    }
}

