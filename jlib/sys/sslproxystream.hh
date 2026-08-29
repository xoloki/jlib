/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2002 Joey Yandle <xoloki@gmail.com>
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
 */

#ifndef JLIB_SYS_SSLPROXYSTREAM_HH
#define JLIB_SYS_SSLPROXYSTREAM_HH

#include <jlib/sys/proxystream.hh>
#include <jlib/sys/sslstream.hh>

namespace jlib {
namespace sys {

/**
 * TLS to a server reached through an HTTP CONNECT proxy.
 *
 * All of this used to be written out again here -- its own SSL_CTX, its own
 * SSL_new, its own SSL_connect, its own underflow and sync -- and the copy had
 * no certificate verification of any kind: no SSL_CTX_set_verify, no trust
 * store, no SSL_set1_host.  So an imaps:// URL with a proxy on it produced a
 * connection that was encrypted and unauthenticated, which any certificate at
 * all would satisfy.  It looked secure, which is the worst way for it to be
 * wrong.
 *
 * It is basic_tlsbuf over basic_proxybuf now, so there is one handshake in the
 * library and the proxy path gets exactly what the direct path gets.  The name
 * to verify against is the *server's*, not the proxy's -- see the note in
 * sslstream.hh.
 */
template<typename charT, typename traitT = std::char_traits<charT> >
using basic_sslproxybuf = basic_tlsbuf<basic_proxybuf<charT,traitT>, charT, traitT>;

template<typename charT, typename traitT=std::char_traits<charT> >
class basic_tlsproxystream : public basic_proxystream<charT,traitT> {
public:
    basic_tlsproxystream()
        : basic_proxystream<charT,traitT>()
    {}

    /**
     * @param host   the server to reach
     * @param port   its port
     * @param phost  the proxy
     * @param pport  the proxy's port
     * @param delay  connect without handshaking; see start()
     */
    basic_tlsproxystream(const std::string& host, unsigned int port,
                         const std::string& phost, u_int pport,
                         bool delay = false)
        : basic_proxystream<charT,traitT>()
    {
        this->m_buf = new basic_sslproxybuf<charT,traitT>(host, delay,
                                                          host, port, phost, pport);
        this->init(this->m_buf);
    }

    void open(const std::string& host, unsigned int port,
              const std::string& phost, u_int pport, bool delay = false)
    {
        if(this->m_buf != 0)
            delete this->m_buf;
        this->m_buf = new basic_sslproxybuf<charT,traitT>(host, delay,
                                                          host, port, phost, pport);
        this->init(this->m_buf);
    }

    /** Handshake now, on a stream opened with delay = true. */
    void start() {
        dynamic_cast< basic_sslproxybuf<charT,traitT>* >(this->m_buf)->start();
    }

};

template<typename charT, typename traitT = std::char_traits<charT> >
using basic_sslproxystream = basic_tlsproxystream<charT,traitT>;

typedef basic_tlsproxystream< char, std::char_traits<char> > tlsproxystream;

/** The older name for a tlsproxystream that handshakes at once. */
typedef tlsproxystream sslproxystream;

}
}

#endif // JLIB_SYS_SSLPROXYSTREAM_HH
