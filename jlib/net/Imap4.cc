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

#include <jlib/net/net.hh>
#include <jlib/net/Imap4.hh>

#include <jlib/sys/sys.hh>
#include <jlib/sys/sslstream.hh>
#include <jlib/sys/sslproxystream.hh>

#include <jlib/util/util.hh>
#include <jlib/util/URL.hh>

#include <algorithm>
#include <sstream>

const int PORT = 143;
const int SSL_PORT = 993;
const std::string SSL_PROTOCOL = "simap";
const std::string OK = "* OK";
const std::string INTERNAL = "Subject: DON'T DELETE THIS MESSAGE -- FOLDER INTERNAL DATA";
const int TAG_WIDTH = 5;
const std::string ENDL = "\r\n";
const unsigned int MAX_CONNECT_ATTEMPTS = 5;

namespace jlib {
    namespace net {


        ListItem::ListItem() {}
        ListItem::ListItem(std::string line) {
            unsigned int i,j;
            i = line.find("(");
            j = line.find(")",i+1);
            
            std::string ending = line.substr(j+1);
            std::vector<std::string> etokens = util::tokenize(ending," ",false);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                std::cout << "ending = '"<<ending<<"'"<<std::endl;
                for(unsigned int i=0;i<etokens.size();i++) {
                    std::cout << "etokens["<<i<<"] = "<<etokens[i]<<std::endl;
                }
            }
            // 
            if(etokens[0][0] == '"') {
                m_delim = util::slice(ending,"\"", "\"");
                std::string b = ending.substr(ending.find(m_delim)+m_delim.length()+2);
                etokens = util::tokenize(b," ",false);
                
                if(etokens[0][0] == '"') {
                    m_name = util::slice(b,"\"", "\"");
                }
                else {
                    m_name = etokens[0];
                }
            }
            else {
                m_delim = etokens[0];
                if(etokens[1][0] == '"') {
                    std::string b = ending;
                    b = b.substr(b.find(m_delim)+m_delim.length()+1);
                    m_name = util::slice(b,"\"", "\"");
                }
                else {
                    m_name = etokens[1];
                }
            }

            m_attr = util::tokenize(line.substr(i+1,j-i-1));
            m_is_folder = true;
            m_is_parent = true;
            for(unsigned int i=0;i<m_attr.size();i++) {
                std::string attr = util::upper(m_attr[i]);
                if(attr.find("NOSELECT") != std::string::npos) {
                    m_is_folder = false;
                }
                else if(attr.find("NOINFERIORS") != std::string::npos) {
                    m_is_parent = false;
                }
            }
            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                std::cout << "m_name = <"<<m_name<<">: m_delim = <"<<m_delim<<">: m_attr = ";
                for(unsigned int i=0;i<m_attr.size();i++)
                    std::cout << "<"<<m_attr[i]<<">";
                std::cout << "is_folder(): " << is_folder() << "; "
                          << "is_parent(): " << is_parent() << std::endl;
                    
                std::cout << std::endl;
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
            return (util::lower(m_url.get_protocol()).find(SSL_PROTOCOL) != std::string::npos);
        }

        Email Imap4::get(sys::socketstream& sock, 
                         int which, 
                         bool only_headers)
        {
            Email ret;
            std::set<Email:: flag_type> flags;
            std::vector<std::string>::iterator i;
            std::string buf;
            std::vector<std::string> info;
            std::vector<std::string> flagv;
            std::string s = util::string_value(which+1);
            std::string flagbuf;
            int size = 0, header_size = 0;

            tag(1);

            std::string req = 
                tag() + " FETCH " + s + ":" + s + " (FLAGS RFC822.SIZE " + 
                (only_headers ? "RFC822.HEADER" : "RFC822") + ")";
            
            if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                std::cout << req << std::endl;

            sock << req << ENDL << std::flush;
            sys::getline(sock, buf);

            if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                std::cout << buf << std::endl;

            if(buf.find(tag()+" NO") == 0) {
                throw exception(buf.substr(tag().length()+1));
            }
            
            info = util::tokenize(buf);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                std::cout << "Response parsed into " << info.size() << " tokens" << std::endl;

            i = find(info.begin(),info.end(), "(FLAGS");
            if(i != info.end()) {
                for(i++; (i != info.end() && *i != "RFC822.SIZE"); i++) {
                    std::string flag = *i;
                    
                    if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                        std::cout << "Found flag: " << flag << std::endl;
                    
                    std::string::size_type x; 
                    while((x=flag.find("(")) != std::string::npos) {
                        flag.erase(x,x+1);
                    }
                    while((x=flag.find(")")) != std::string::npos) {
                        flag.erase(x,x+1);
                    }
                    
                    flagv.push_back(flag);
                }
            }

            i = find(info.begin(),info.end(), "RFC822.SIZE");
            if(i == info.end()) {
                if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                    std::cout << "Didn't find token RFC822.SIZE, look for (RFC822.SIZE" << std::endl;

                i = find(info.begin(),info.end(), "(RFC822.SIZE");
            }

            if(i != info.end() && (++i) != info.end()) {
                if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                    std::cout << "Found RFC822.SIZE:" << *i << std::endl;

                size = util::int_value(*i);
            } else {
                if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                    std::cout << "Didn't find tokens RFC822.SIZE or (RFC822.SIZE" << std::endl;
            }

            if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                std::cout << "Parse header size from token" << info.back() << std::endl;

            //header_size = util::int_value(util::slice(info[info.size()-1], "{", "}"));
            header_size = util::int_value(util::slice(info.back(), "{", "}"));
            if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                std::cout << "Header size:" << header_size << std::endl;
            
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << "reading " << header_size << "bytes..." << std::flush;
            sys::getstring(sock, buf, header_size);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << "done" << std::endl;

            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                std::cout << buf << std::endl;
            }
            
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << "creating email from buffer with size " << size << std::endl;
            try { 
                ret.create(buf); 
            } 
            catch(std::exception& e) {
                if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                    std::cout << "caught exception while creating email: " << e.what() << std::endl;
            }
            catch(...) {
                if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                    std::cout << "caught exception while creating email " << size << std::endl;
            }
            ret.set_data_size(size);

            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                std::cout << "iterating over flagv: size " << flagv.size() << std::endl;
            }

            for(i = flagv.begin(); i != flagv.end(); i++) {
                if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                    std::cout << "checking flag " << *i << std::endl;
                }
                
                if(*i == "\\Seen") {
                    flags.insert(flags.begin(), Email::seen_flag);
                }
                else if(*i == "\\Answered") {
                    flags.insert(flags.begin(), Email::answered_flag);
                }
                else if(*i == "\\Deleted") {
                    flags.insert(flags.begin(), Email::deleted_flag);
                }
            }

            if(!only_headers) {
                flags.insert(flags.begin(), Email::seen_flag);
            }

            ret.set_flags(flags);

            while(!util::begins(buf, tag())) {
                sys::getline(sock, buf);
                if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << buf << std::endl;
            }
            
            if(buf.find(tag()+" NO") == 0 || buf.find(tag()+" BAD") == 0) {
                throw exception(buf.substr(tag().length()+1));
            }

            return ret;            
            
        }

        std::string Imap4::retrieve_headers(unsigned int which, 
                                       std::string mailbox,
                                       unsigned int& size) throw(std::exception)
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
                                       std::string mailbox,
                                       unsigned int& size) throw(std::exception) {

            std::string buf;
            //std::vector<std::string> info;
            std::string s = util::valueOf(which+1);
            //info = handshake(sock"FETCH "+s+":"+s+" (FLAGS RFC822)");
            tag(1);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << std::string(tag()+" FETCH "+s+":"+s+" (FLAGS RFC822.HEADER)") << std::endl;
            sock << std::string(tag()+" FETCH "+s+":"+s+" (FLAGS RFC822.SIZE RFC822.HEADER)") << ENDL << std::flush;
            sys::getline(sock, buf);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << buf << std::endl;
            if(buf.find(tag()+" NO") == 0) {
                throw exception(buf.substr(tag().length()+1));
            }
            std::vector<std::string> bufvec = util::tokenize(buf);

            std::vector<std::string>::iterator i = find(bufvec.begin(),bufvec.end(), "RFC822.SIZE");
            if(i != bufvec.end() && (++i) != bufvec.end()) {
                size = util::int_value(*i);
            }
            else {
                size = 0;
            }

            long n = util::intValue(util::slice(bufvec[bufvec.size()-1], "{", "}"));
            
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << "DEBUG: reading " << n << " bytes from server...\n";            
            std::string ret;
            sys::getstring(sock, ret, n);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << ret << std::endl;
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << "DEBUG: finished reading\n";
            
            while(!util::begins(buf, tag())) {
                sys::getline(sock, buf);
                if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << buf << std::endl;
            }
            
            if(buf.find(tag()+" NO") == 0 || buf.find(tag()+" BAD") == 0) {
                throw exception(buf.substr(tag().length()+1));
            }

            return ret;            
        }

        std::string Imap4::retrieve(int which, std::string mailbox) throw(std::exception) {
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

        std::string Imap4::retrieve(sys::socketstream& sock, int which, std::string mailbox) throw(std::exception) 
        {
            std::string buf;
            //std::vector<std::string> info;
            std::string s = util::valueOf(which+1);
            //info = handshake(sock"FETCH "+s+":"+s+" (FLAGS RFC822)");
            sock << std::string(tag(1)+" FETCH "+s+":"+s+" (FLAGS RFC822)") << ENDL << std::flush;
            sys::getline(sock, buf);
            if(buf.find(tag()+" NO") == 0) {
                throw exception(buf.substr(tag().length()+1));
            }
            std::vector<std::string> bufvec = util::tokenize(buf);
            long n = util::intValue(util::slice(bufvec[bufvec.size()-1], "{", "}"));
            
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << "DEBUG: reading " << n << " bytes from server...\n";            
            std::string ret;
            sys::getstring(sock, ret, n);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << ret << std::endl;
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << "DEBUG: finished reading\n";
            
            while(!util::begins(buf, tag())) {
                sys::getline(sock, buf);
                if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << buf << std::endl;
            }
            
            if(buf.find(tag()+" NO") == 0 || buf.find(tag()+" BAD") == 0) {
                throw exception(buf.substr(tag().length()+1));
            }

            std::pair<unsigned int, unsigned int> p(which+1,which+1);
            std::vector<std::string> f; f.push_back("\\Seen");
            store(sock,p,"+FLAGS.SILENT",f);
            
            return ret;
        }
        
        sys::socketstream* Imap4::connect() throw(std::exception) {
            sys::socketstream* sock = 0;
            if(getenv("JLIB_NET_IMAP4_DEBUG")) 
                std::cout << "begin opening "<<m_host<<" on port "<<m_port<<"... "<<std::endl;
            unsigned int i=0;
            while(i<MAX_CONNECT_ATTEMPTS && sock == 0) {
                try {
                    std::string phost;
                    u_int pport;

                    if(m_url["proxy"] != "") {
                        std::vector<std::string> pvec = 
                            util::tokenize(m_url["proxy"], ":");
                        phost = pvec[0];
                        pport = util::int_value(pvec[1]);
                        if(getenv("JLIB_NET_PROXY_DEBUG")) 
                            std::cout << "proxy "<<phost<<" on port "<<pport<<std::endl;
                    }

                    if(is_secure()) {
                        if(m_url["proxy"] != "") {
                            sock = new sys::sslproxystream(m_host,
                                                                 m_port,
                                                                 phost, 
                                                                 pport);
                        } else {
                            sock = new sys::sslstream(m_host,m_port);
                        }
                    }
                    else {
                        if(m_url["proxy"] != "") {
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
            std::string buf;
            sys::getline(*sock, buf);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) std::cout << "read first line: " << buf << std::endl;
            if(!util::begins(buf, OK)) {
                throw exception("error connecting: expected '"+OK+"', received "+buf);
            }
            m_state = NonAuthenticated;
            return sock;
            //handshake(sock"LOGIN "+m_user+" "+m_pass);
        }
        
        void Imap4::disconnect(sys::socketstream& sock) throw(std::exception) {
            //handshake(sock"CLOSE");
            //handshake(sock"LOGOUT");
            sock.close();
            m_state = UnConnected;
        }
        
        void Imap4::remove(int which, std::string mailbox) throw(std::exception) {
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
        bool Imap4::internal(std::string data) {
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

        std::vector<std::string> Imap4::handshake(sys::socketstream& sock, std::string data) throw(std::exception) {
            std::string buf;
            std::string com = tag(1)+" "+data;
            std::vector<std::string> ret;
            bool idle = (data == "IDLE");

            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                if(util::upper(data.substr(0,5)).find("LOGIN") == 0) {
                    std::vector<std::string> tok = util::tokenize(data);
                    if(tok.size() >= 2) {
                        std::cout << tag()<<" "<<tok[0] << " " << tok[1] << " ********"<<std::endl;
                    }
                    else {
                        std::cout << tag() << "LOGIN **** ********"<<std::endl;
                    }
                }
                else {
                    std::cout << com << std::endl;
                }
            }
            sock << com << ENDL << std::flush;

            std::string end = (idle ? "+" : tag());

            while(!util::begins(buf, end)) {
                sys::getline(sock, buf);
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
                    sys::getline(sock, buf);
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
            for(unsigned int i=0;i<hand.size();i++) {
                if(util::icontains(hand[i], "exists")) {
                    std::vector<std::string> tok = util::tokenize(hand[i]);
                    exists(util::int_value(tok[1]));
                }
                else if(util::icontains(hand[i], "recent")) {
                    std::vector<std::string> tok = util::tokenize(hand[i]);
                    recent(util::int_value(tok[1]));
                }
                else if(util::icontains(hand[i], "unseen")) {
                    std::vector<std::string> tok = util::tokenize(hand[i]);
                    unseen(util::int_value(tok[2]));
                }
            }
        }


        std::vector<std::string> Imap4::capability(sys::socketstream& sock) {
            std::vector<std::string> cap = handshake(sock,"CAPABILITY");
            std::vector<std::string> ret = util::tokenize(cap[0]);
            ret.erase(ret.begin(),ret.begin()+2);
            return ret;
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

        void Imap4::logout(sys::socketstream& sock) throw(std::exception) {
            handshake(sock,"LOGOUT");
            m_state = UnConnected;
        }
        void Imap4::authenticate(sys::socketstream& sock,std::string name) {
            handshake(sock,"AUTHENTICATE "+name);
        }
        void Imap4::login(sys::socketstream& sock, std::string user, std::string pass) throw(std::exception) {
            if(user!="" && pass != "") {
                handshake(sock,"LOGIN "+user+" "+pass);
            }
            else {
                handshake(sock,"LOGIN "+m_user+" "+m_pass);            
            }
            m_state = Authenticated;
        }

        std::vector<std::string> Imap4::select(sys::socketstream& sock, std::string path) {
            std::vector<std::string> config = handshake(sock,"SELECT \""+path+"\"");
            parse(config);
            m_state = Selected;
            return config;
        }
        
        std::vector<std::string> Imap4::examine(sys::socketstream& sock, std::string path) {
            std::vector<std::string> ret = handshake(sock,"EXAMINE \""+path+"\"");
            parse(ret);
            m_state = Selected;
            return ret;
        }

        void Imap4::create(sys::socketstream& sock, std::string path) {
            handshake(sock,"CREATE \""+path+"\"");
        }
        void Imap4::remove(sys::socketstream& sock, std::string path) {
            handshake(sock,"DELETE \""+path+"\"");
        }
        void Imap4::rename(sys::socketstream& sock, std::string old_name, std::string new_name) {
            handshake(sock,"RENAME \""+old_name+"\" \""+new_name+"\"");
        }
        void Imap4::subscribe(sys::socketstream& sock, std::string path) {
            handshake(sock,"SUBSCRIBE \""+path+"\"");
        }
        void Imap4::unsubscribe(sys::socketstream& sock, std::string path) {
            handshake(sock,"UNSUBSCRIBE \""+path+"\"");
        }

        std::vector<ListItem> Imap4::list(sys::socketstream& sock, std::string ref, std::string path) {
            std::vector<ListItem> ret;
            std::vector<std::string> ls = handshake(sock,"LIST \""+ref+"\" \""+path+"\"");
            for(unsigned int i=0;i<ls.size()-1;i++) {
                ret.push_back(ListItem(ls[i]));
            }
            return ret;
        }

        std::vector<ListItem> Imap4::lsub(sys::socketstream& sock, std::string ref, std::string path) {
            std::vector<ListItem> ret;
            std::vector<std::string> ls = handshake(sock,"LIST \""+ref+"\" \""+path+"\"");
            for(unsigned int i=0;i<ls.size()-1;i++) {
                ret.push_back(ListItem(ls[i]));
            }
            return ret;
        }

        void Imap4::append(sys::socketstream& sock, std::string path, std::string data, std::string flag, std::string date) {
            std::string buf;
            path = (path == "INBOX" ? path : (m_url.get_path_no_slash() + path));
            tag(1);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                std::cout << tag() << " APPEND \""<<path<<"\" ("<<flag<<") {"<<data.length()<<"}"<<std::endl;
            }
            sock << tag() << " APPEND \""<<path<<"\" ("<<flag<<") {"<<data.length()<<"}"<<ENDL << std::flush;
            sys::getline(sock,buf);
            if(getenv("JLIB_NET_IMAP4_DEBUG")) {
                std::cout <<buf<<std::endl;
            }
            // TODO: figure out why the hell I'm getting an 'm' character before the '+'
            if(!buf.find("+") == buf.npos) {
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

        std::vector<std::string> Imap4::search(sys::socketstream& sock, std::string criteria, std::string spec) {
            std::vector<std::string> ret = handshake(sock,"");
            return ret;
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

        std::vector<std::string> Imap4::partial(sys::socketstream& sock, std::pair<unsigned int,unsigned int> set, std::vector<std::string> n,unsigned int p, unsigned int o) {
            std::vector<std::string> ret = handshake(sock,"");
            return ret;            
        }

        std::vector<std::string> Imap4::store(sys::socketstream& sock, std::pair<unsigned int,unsigned int> set, std::string key, std::vector<std::string> val) {
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

        void Imap4::copy(sys::socketstream& sock, std::pair<unsigned int,unsigned int> set, std::string box) {
            std::string path = (box == "INBOX" ? box : (m_url.get_path_no_slash() + box));
            std::ostringstream cmd;
            cmd << "COPY " << set.first << ":" << set.second << " \"" << path << "\"";
            std::vector<std::string> ret = handshake(sock,cmd.str());
        }
        
        std::vector<std::string> Imap4::uid(sys::socketstream& sock, std::string cmd, std::vector<std::string> arg) {
            std::vector<std::string> ret = handshake(sock,"");
            return ret;
        }
       
    }
}
