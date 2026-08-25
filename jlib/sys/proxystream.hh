/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_SYS_PROXYSTREAM_HH
#define JLIB_SYS_PROXYSTREAM_HH

#include <jlib/sys/socketstream.hh>
#include <jlib/util/util.hh>

#include <openssl/ssl.h>

#include <sstream>

namespace jlib {
namespace sys {

template< typename charT, typename traitT = std::char_traits<charT> >
class basic_proxybuf : public basic_socketbuf<charT,traitT> {
public:
    typedef charT 					            char_type;
    typedef traitT 					            traits_type;
    typedef typename traits_type::int_type 		int_type;
    typedef typename traits_type::pos_type 		pos_type;
    typedef typename traits_type::off_type 		off_type;
    
    static const unsigned int BUF_SIZE = 1024;
    
    basic_proxybuf(const std::string& host, u_int port,
                   const std::string& phost, u_int pport) 
        : basic_socketbuf<charT,traitT>(phost,pport),
        m_proxy_host(host),
        m_proxy_port(port)
    {
        open_proxy();
    }
    
    virtual ~basic_proxybuf() {
        if(getenv("JLIB_SYS_SOCKET_DEBUG"))
            std::cerr << "basic_proxybuf::~basic_proxybuf()"<<std::endl;
        this->close();
    }
    
protected:
    void open_proxy() {
        std::ostringstream os, con;

        os << m_proxy_host << ":" << m_proxy_port;
        con << "CONNECT " << os.str() << " HTTP/1.1\r\n"
            << "Host: " << os.str() << "\r\n"
            << "\r\n";

        const std::string connect(con.str());

        if(getenv("JLIB_SYS_PROXY_DEBUG"))
            std::cerr << connect << std::flush;

        this->sputn(connect.data(), connect.length());

        // this->sync(), not sync().
        //
        // basic_socketbuf is a dependent base, so unqualified lookup does not
        // find its members -- and there is a ::sync() in <unistd.h>, which
        // takes no arguments, returns void, and flushes the machine's
        // filesystem buffers.  So this compiled, called that, and never sent
        // the request.  The proxy support has never worked; the line above it
        // gets this->sputn right, so someone knew and missed one.
        this->sync();

        // The whole of the response head, to the blank line.  What was here
        // read until it had seen two '\n' characters and then threw the result
        // away -- so a proxy that sends any headers of its own (most do:
        // Proxy-agent, Connection) left the rest of them in the stream to be
        // read as protocol data, and a proxy that refused the tunnel was
        // indistinguishable from one that granted it.
        std::string head;

        while(head.find("\r\n\r\n") == std::string::npos &&
              head.find("\n\n") == std::string::npos) {
            const int c = this->sbumpc();

            if(c == traits_type::eof()) {
                throw typename basic_socketbuf<charT,traitT>::exception(
                    "the proxy closed the connection before answering CONNECT: "
                    "read \"" + head + "\"");
            }

            head.append(1, static_cast<char>(c));

            if(head.size() > 8192) {
                throw typename basic_socketbuf<charT,traitT>::exception(
                    "the proxy's answer to CONNECT has no end to its headers");
            }
        }

        if(getenv("JLIB_SYS_PROXY_DEBUG"))
            std::cerr << head << std::flush;

        // RFC 9110 9.3.6: any 2xx means the tunnel is up, and anything else
        // means it is not.  "HTTP/1.1 200 Connection established".
        const std::string::size_type sp = head.find(' ');

        if(sp == std::string::npos || head.compare(0, 5, "HTTP/") != 0) {
            throw typename basic_socketbuf<charT,traitT>::exception(
                "the proxy did not answer CONNECT with a status line: " + head);
        }

        if(head[sp + 1] != '2') {
            const std::string::size_type nl = head.find_first_of("\r\n");

            throw typename basic_socketbuf<charT,traitT>::exception(
                "the proxy refused CONNECT: " + head.substr(0, nl));
        }
    }

    std::string m_proxy_host;
    uint m_proxy_port;
};
        
template<typename charT, typename traitT=std::char_traits<charT> >
class basic_proxystream : public basic_socketstream<charT,traitT> {
public:
    basic_proxystream()
        : basic_socketstream<charT,traitT>()
    {}
    
    basic_proxystream(const std::string& host, unsigned int port,
                      const std::string& phost, u_int pport) 
        : basic_socketstream<charT,traitT>()
    {
        this->m_buf=new basic_proxybuf<charT,traitT>(host,port,phost,pport);
        this->init(this->m_buf);
    }
            
    void open(const std::string& host, unsigned int port,
              const std::string& phost, u_int pport) 
    {
        this->m_buf=new basic_proxybuf<charT,traitT>(host,port,phost,pport);
        this->init(this->m_buf);
    }

};
    
    typedef basic_proxystream< char, std::char_traits<char> > proxystream;
        
}
}


#endif // JLIB_SYS_PROXYSTREAM_HH
