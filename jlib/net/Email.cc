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

#include <jlib/net/net.hh>
#include <jlib/net/content_type.hh>

#include <jlib/sys/sys.hh>

#include <jlib/util/Date.hh>
#include <jlib/util/Regex.hh>
#include <jlib/util/util.hh>

#include <iomanip>
#include <sstream>
#include <algorithm>

namespace jlib {
    namespace net {

        Email::Email() 
            : m_is_loaded(false),
              m_indx(-1)
        { 
            m_sort = "DATE"; 
        }

        Email::Email(const std::string& is) 
            : m_is_loaded(false),
              m_indx(-1)
        {
            create(std::move(is));
        }

        void Email::create(const std::string& is) {
            if(getenv("JLIB_NET_EMAIL_DEBUG")) {
                std::cerr <<"jlib::net::Email::create(): entering"<<std::endl;
            }
            m_sort = "DATE";
            if(getenv("JLIB_NET_EMAIL_DEBUG")) {
                std::cerr <<"jlib::net::Email::create(): calling parse_end(is,<";
                for(unsigned int q=0;q<m_bounds.size();q++) {
                    std::cerr <<m_bounds[q];
                    if(q+1 < m_bounds.size()) {
                        std::cerr << ",";
                    }
                }
                std::cerr << ">, \"" << m_raw << "\") " << std::endl;
            }
            // Moved: is is not read again below, and it is the whole message.
            m_raw = std::move(is);// = parse_end(is, m_bounds);
            if(getenv("JLIB_NET_EMAIL_DEBUG")) {
                std::cerr <<"jlib::net::Email::create(): after parse_end(),"
                          <<"m_raw => \n" << m_raw << std::endl;
            }
            if(getenv("JLIB_NET_EMAIL_DEBUG")) {
                std::cerr <<"jlib::net::Email::create(): calling m_headers.parse()"
                          << std::endl;
            }
            m_headers.parse(m_raw);
            if(getenv("JLIB_NET_EMAIL_DEBUG")) {
                std::cerr <<"jlib::net::Email::create(): after m_headers.parse(), "
                          <<"we have the following headers:\n";
                std::cerr << std::string(m_headers) << std::endl;
            }

            // TODO: this is fucked up when we're coming from MBoxBuffer
            std::string::size_type header_end = m_headers.get_length();

            if(getenv("JLIB_NET_EMAIL_DEBUG")) {
                std::cerr <<"jlib::net::Email::create(): header_end="<<header_end;
                if(header_end == m_raw.npos) {
                    std::cerr << "; header_end == m_raw.npos";
                }
                
                std::cerr << std::endl;
            }
            //std::istringstream ireceived(m_raw);
            //m_received = parse_header(ireceived,"received");
            parse_received();
            
            // sanitize headers
            if(find("CONTENT-TYPE") == "" ) {
                if(getenv("JLIB_NET_EMAIL_DEBUG")) {
                    std::cerr <<"jlib::net::Email::create(): unknown Content-Type, assuming text/plain"
                              << std::endl;
                }
                set("CONTENT-TYPE","text/plain");
            }

            std::string type = find("CONTENT-TYPE");

            if(getenv("JLIB_NET_EMAIL_DEBUG")) {
                std::cerr <<"jlib::net::Email::create(): Content-Type: "
                          << type << std::endl;
            }
            // icontains(type, "multipart") used to be the test, so a
            // Content-Type of text/plain; name="multipart.txt" was a multipart
            // message.  is() compares the type, not the header.
            net::content_type ctype;

            try {
                ctype = net::content_type::parse(type);
            }
            catch(net::content_type::exception& e) {
                // An unreadable Content-Type is text/plain, which is what
                // RFC 2045 5.2 says to do with one that is missing, and is
                // what the sanitizing above already assumes.
                if(getenv("JLIB_NET_EMAIL_DEBUG")) {
                    std::cerr << "jlib::net::Email::create(): " << e.what()
                              << std::endl;
                }
            }

            const std::string bound = ctype.get("boundary");

            if(ctype.is("multipart") && !bound.empty()) {
                // The boundary used to be found by splitting the header on
                // ";" -- which splits inside a quoted value, so boundary="a;b"
                // came out as boundary="a -- and then asking util::slice for
                // what lay between the quotes, which returns its whole input
                // when there are none.  Neither failure was visible: the
                // message simply did not split into its parts, so an
                // attachment went missing or the body was shown as one.
                //
                // The split is RFC 2046 5.1.1 now, rather than parse_divide on
                // "--" + bound.  It is the leading CRLF belonging to the
                // delimiter that this gets right and the old loop did not.
                for(std::string& part : net::split_multipart(m_raw.substr(header_end),
                                                             bound)) {
                    m_attach.push_back(Email(std::move(part)));
                }
            }
            else if(ctype.is("message", "rfc822")) {
                // The two empty branches that used to sit beside this one
                // tested for "message" and then for "message/digest", which is
                // not a media type -- the digest is multipart/digest, and it
                // is handled by the multipart branch above.
                m_attach.push_back(Email(m_raw.substr(header_end)));
            }
            else {
                std::string encoding = jlib::util::upper(find("CONTENT-TRANSFER-ENCODING"));

                // this is the end of the headers.  it's also possible to
                // take ihead and grab the rest of it, but this is probaby faster
                if(encoding.find("BASE64") != encoding.npos) {
                    if(header_end != m_raw.npos) {
                        m_data = jlib::util::base64::decode(m_raw.substr(header_end));
                    }
                }
                else if(encoding.find("QUOTED-PRINTABLE") != encoding.npos) {
                    if(header_end != m_raw.npos) {
                        m_data = jlib::util::qp::decode(m_raw.substr(header_end));
                    } 
                }
                else {
                    if(header_end != m_raw.npos) {
                        m_data = m_raw.substr(header_end);
                    }                    
                    
                }
            }
            if(getenv("JLIB_NET_EMAIL_DEBUG")) {
                std::cerr <<"jlib::net::Email::create(): leaving"<<std::endl;
            }
        }
        
        bool operator<(const Email& j1, const Email& j2) {
            std::string s1 = j1[j1.m_sort];
            std::string s2 = j2[j2.m_sort];
            
            if(j1.m_sort == "DATE" && j2.m_sort == "DATE") {
                jlib::util::Date d1; d1.set(s1);
                jlib::util::Date d2; d2.set(s2);
                //cout << d1.time() << " -- " << d2.time() << endl;
                return d1.time() < d2.time();
            }
            else if(j1.m_sort == "SIZE" && j2.m_sort == "SIZE") {
                return (j1.get_data_size() < j2.get_data_size());
            }
            //cout << "Testing operator< on '" << s1.c_str() << "' and '" << s2.c_str() << "'\n";
            return (s1 < s2);
        }
        
        void Email::sort(const std::string& field) {
            m_sort = std::move(field);
        }
        
        std::vector<Email>& Email::attach() {
            return m_attach;
        }
        
        bool Email::internal() const {
            std::string subject = find("SUBJECT");
            if(subject != "") {
                return jlib::util::icontains(subject,"FOLDER INTERNAL DATA");
            }
            else {
                return false;
            }
        }

        void Email::build() {
            build_mime(m_raw, *this);
        }

        std::string Email::operator[](const std::string& key) const {
            return m_headers[key];
        }

        void Email::set(const std::string& key,std::string val) {
            //std::multimap<std::string,std::string>::const_iterator i = m_headers.lower_bound(key);
            m_headers.set(key,val);
        }

        void Email::add(const std::string& key,std::string val) {
            m_headers.add(key,val);
        }

        void Email::set_flag(flag_type flag) {
            std::set<flag_type> f; f.insert(f.begin(), flag);
            set_flags(f);
        }

        void Email::unset_flag(flag_type flag) {
            std::set<flag_type> f; f.insert(f.begin(), flag);
            unset_flags(f);
        }

        void Email::set_flags(std::set<flag_type> flags) {
            std::copy(flags.begin(),flags.end(), inserter(m_flags,m_flags.begin()));
        }

        void Email::unset_flags(std::set<flag_type> flags) {
            std::set<flag_type>::iterator i = flags.begin();
            for(;i!=flags.end();i++) {
                std::set<flag_type>::iterator j = m_flags.find(*i);
                if(j != m_flags.end()) {
                   m_flags.erase(j);
                }
            }
        }

        void Email::clear_flags() {
            m_flags.clear();
        }

        std::string Email::get_received_ip() const {
            jlib::util::Regex ipexp("([[:digit:]]{1,3}\\.[[:digit:]]{1,3}\\.[[:digit:]]{1,3}\\.[[:digit:]]{1,3})");
            std::vector<std::string>::const_reverse_iterator r = m_received.rbegin();

            while(r != m_received.rend() && (!ipexp(*r) || is_reserved(ipexp[1])) ) r++;

            if(r != m_received.rend()) {
                return ipexp[1];
            }
            else {
                return "";
            }

        }

        std::string Email::get_primary_text() const { return get_text(false,false,false,true); }
        std::string Email::get_primary_text_html() const { return get_text(true,false,false,true); }
        std::string Email::get_primary_text_html_render() const { return get_text(true,true,false,true); }

        std::string Email::get_globbed_text() const { return get_text(false,false,true,true); }
        std::string Email::get_globbed_text_html() const { return get_text(true,false,true,true); }
        std::string Email::get_globbed_text_html_render() const { return get_text(true,true,true,true); }
        
        std::string Email::get_text(bool html, bool render, bool globbed, bool recurse) const {
            std::string ctype = jlib::util::lower(find("CONTENT-TYPE"));
            std::string globret = "["+ctype+"]\n\n";
            std::string ret;

            /*
             * first, get the text of the current chunk.  for multipart/alternative,
             * decide between pieces.  for all others, simply display text if available.
             * 
             */
            if(globbed) {
                if(ctype == "" || ctype.find("message") == 0 || ctype.find("text") == 0) {
                    ret = data();
                    if(ctype.find("text/html") == 0 && render)
                        ret = jlib::net::html::render(ret);
                }
                else {
                    
                }
                
            }
            else {
                if(ctype.find("multipart/alternative") == 0) {
                    std::string textpart, htmlpart, texttype, htmltype;
                    for(const_iterator i = begin();i != end(); i++) {
                        std::string subctype = jlib::util::lower(i->find("CONTENT-TYPE"));
                        if(subctype.find("text/plain") == 0) {
                            textpart = i->data();
                            texttype = "["+subctype+"]\n\n";
                        }
                        else if(subctype.find("text/html") == 0) {
                            htmlpart = i->data();
                            if(render)
                                htmlpart = jlib::net::html::render(htmlpart);
                            htmltype = "["+subctype+"]\n\n";
                        }
                    }
                    if(html && htmlpart != "") {
                        ret = htmlpart;
                    }
                    else {
                        ret = textpart;
                    }
                }
                else if(ctype == "" || ctype.find("message") == 0 || ctype.find("text") == 0) {
                    ret = data();
                    if(ctype.find("text/html") == 0 && render)
                        ret = jlib::net::html::render(ret);
                    
                }
            }

            if(globbed) {
                ret = globret + ret;
            }

            if(recurse) {
                for(const_iterator i = begin();i != end(); i++) {
                    if(globbed || ctype.find("multipart/alternative") == std::string::npos)
                        ret += (i->get_text(html,render,globbed,recurse));
                }
            }

            return ret;
        }

        bool Email::is(const std::string& type) const {
            // A whole-component comparison.  This used to be find() != npos
            // over the entire lowercased header, so is("text") was true for
            // application/x-latext, and true for any multipart whose boundary
            // happened to contain the letters "text" -- which is to say, true
            // for a message whose boundary the sender chose.
            const std::string::size_type slash = type.find('/');

            try {
                const net::content_type c = net::content_type::parse(find("CONTENT-TYPE"));

                if(slash == type.npos) return c.is(type);

                return c.is(type.substr(0, slash), type.substr(slash + 1));
            }
            catch(net::content_type::exception&) {
                return false;
            }
        }

        void Email::parse_received() {
            if(getenv("JLIB_NET_EMAIL_DEBUG")) 
                std::cerr <<"jlib::net::parse_received(): entering"<<std::endl;
            m_received.clear();
            std::string key = "RECEIVED";
            jlib::util::Headers::list_type keys = m_headers.keys();
            jlib::util::Headers::list_type vals = m_headers.vals(key);
            
            jlib::util::Headers::list_type::iterator i = std::find(keys.begin(),keys.end(),key),
                j=vals.begin();

            while(i != keys.end() && *i == key) {
                if(getenv("JLIB_NET_EMAIL_DEBUG"))
                    std::cerr <<"\nadding ip "<<*j << std::endl;
                m_received.push_back(*j);
                i++;
                j++;
            }
            
        }


        bool Email::is_loaded() const {
            return m_is_loaded;
        }

        void Email::set_loaded(bool b) {
            m_is_loaded = b;
        }

        std::string Email::get_name() const {
            // The old version matched a piece that *began* with "name=", so a
            // parameter written with a space before it was invisible, and it
            // read the value with util::slice, which hands back its whole
            // input when the quotes are not there.  RFC 2231's filename*= form
            // -- which is how any name with a non-ASCII character in it
            // arrives -- it did not know about at all.
            try {
                return net::content_type::parse(find("CONTENT-TYPE")).get("name");
            }
            catch(net::content_type::exception&) {
                return std::string();
            }
        }

        std::string Email::get_filename() const {
            // Content-Disposition, RFC 2183, and the same grammar -- the
            // disposition type is a token with the parameter syntax of
            // RFC 2045 after it.
            try {
                const std::string name =
                    net::content_type::parse_disposition(find("CONTENT-DISPOSITION"))
                        .get("filename");

                if(!name.empty()) return name;
            }
            catch(net::content_type::exception&) {
            }

            // Falling back to Content-Type's "name" is new.  Every mail client
            // does it, because plenty of senders put the attachment's name
            // there and nowhere else, and an attachment that displays with no
            // name is the whole of what this function is for.
            return get_name();
        }

        void Email::set_indx(int indx) {
            m_indx = indx;
        }

        int Email::get_indx() const {
            return m_indx;
        }

        Email::reference Email::grep(const std::string& s, bool recursive) {
            std::string::size_type i;
            if((i = m_data.find(s)) != std::string::npos) {
                return *this;
            }

            if(recursive) {
                for(iterator i = begin(); i != end(); i++) {
                    try {
                        reference r = i->grep(s, true);
                        return r;
                    } catch(exception& e) {}
                }
            }

            throw exception("unable to find \""+s+"\"");
        }

    }
}

