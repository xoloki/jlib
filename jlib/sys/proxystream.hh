/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2000 Joe Yandle <jwy@divisionbyzero.com>
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
    
    basic_proxybuf(std::string host, u_int port,
                   std::string phost, u_int pport) 
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
        std::string buf;
        std::ostringstream os, con;
        int c;
        int n;
        os << m_proxy_host << ":" << m_proxy_port;
        con << "CONNECT " << os.str() << " HTTP/1.1\r\n"
            << "Connection: persist\r\n\r\n";
        
        std::string connect(con.str());
        
        if(getenv("JLIB_SYS_PROXY_DEBUG"))
            std::cerr << connect <<std::flush;
        
        this->sputn(connect.data(), connect.length());
        sync();
        
        //c = sgetc();
        c = this->sbumpc();
        if(c == '\n') n++;
        for(n = 0; (n < 2 && c != -1); ) {
            if(getenv("JLIB_SYS_PROXY_DEBUG")) {
                if(c == '\r') {
                    std::cerr << "\\r" << std::flush;
                } else if(c == '\n') {
                    std::cerr << "\\n\n" << std::flush;
                } else {
                    std::cerr << (char)c << std::flush;
                }
            }
            buf.append(1, (char)c);
            //c = sgetc();
            c = this->sbumpc();
            if(c == '\n') n++;
        }
        if(getenv("JLIB_SYS_PROXY_DEBUG") && c != -1) {
            buf.append(1, (char)c);
            std::cerr << "\\n" << std::endl;
        }
                
        if(getenv("JLIB_SYS_PROXY_DEBUG")) {
            if(c != -1) {
                /*
                  while((c=snextc()) != -1) {
                  if(c == '\r') {
                  std::cerr << "\\r" << std::flush;
                  } else if(c == '\n') {
                  std::cerr << "\\n\n" << std::flush;
                  } else {
                  std::cerr << (char)c << std::flush;
                  }
                  }
                */
            } else {
                std::cerr << "reached EOF after reading '" << buf << "'" << std::endl;
            }
        }

        //n = sgetn(buffer, 4096);

        //if(getenv("JLIB_SYS_PROXY_DEBUG"))
        //std::cerr << buf <<std::endl;
                
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
    
    basic_proxystream(std::string host, unsigned int port,
                      std::string phost, u_int pport) 
        : basic_socketstream<charT,traitT>()
    {
        this->m_buf=new basic_proxybuf<charT,traitT>(host,port,phost,pport);
        this->init(this->m_buf);
    }
            
    void open(std::string host, unsigned int port,
              std::string phost, u_int pport) 
    {
        this->m_buf=new basic_proxybuf<charT,traitT>(host,port,phost,pport);
        this->init(this->m_buf);
    }

};
    
    typedef basic_proxystream< char, std::char_traits<char> > proxystream;
        
}
}


#endif // JLIB_SYS_PROXYSTREAM_HH
