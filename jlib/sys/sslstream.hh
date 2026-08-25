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

#ifndef JLIB_SYS_SSLSTREAM_HH
#define JLIB_SYS_SSLSTREAM_HH

#include <jlib/sys/socketstream.hh>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <sstream>

namespace jlib {
    namespace sys {

        /**
         * TLS over any already-connected socket buffer.
         *
         * Base is the buffer that gets the connection open: basic_socketbuf
         * for a direct connection, basic_proxybuf for one through an HTTP
         * CONNECT proxy.  Everything from the handshake up is the same either
         * way, and used to be written twice -- once here and once in
         * sslproxystream.hh, where the copy had no certificate verification at
         * all.  An imaps:// URL with a proxy was encrypted and unauthenticated.
         *
         * The host to verify against is passed in rather than taken from the
         * base, because for a proxy connection the base's m_host is the
         * *proxy's* name.  The certificate has to belong to the server that
         * was asked for.
         */
        template< typename Base, typename charT, typename traitT = std::char_traits<charT> >
        class basic_tlsbuf : public Base {
        public:
            typedef charT 					            char_type;
            typedef traitT 					            traits_type;
            typedef typename traits_type::int_type 		int_type;
            typedef typename traits_type::pos_type 		pos_type;
            typedef typename traits_type::off_type 		off_type;
            
            static const unsigned int BUF_SIZE = 1024;

            /**
             * @param verify_host the name the certificate must be good for
             * @param delay       connect without handshaking; see start()
             * @param args        whatever Base's constructor takes
             */
            template<typename... Args>
            basic_tlsbuf(const std::string& verify_host, bool delay, Args&&... args)
                : Base(std::forward<Args>(args)...),
                  m_ctx(0),
                  m_ssl(0),
                  m_verify_host(verify_host),
                  m_delay(delay)
            {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_tlsbuf::basic_tlsbuf(" << verify_host << ", "
                              << std::boolalpha << delay << ")"<<std::endl;
                if(!m_delay)
                    open_ssl();
            }

            virtual ~basic_tlsbuf() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_tlsbuf::~basic_tlsbuf()"<<std::endl;
                close();
            }

            virtual int_type underflow() {
                if(m_delay)
                    return Base::underflow();

                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_tlsbuf::underflow()"<<std::endl;

                this->m_eintr = false;
                int count = SSL_read(m_ssl, this->eback(), BUF_SIZE);
                
                if(count < 0) {
                    //throw exception("error reading");
                    //this->setstate(std::ios_base::badbit);
                    if(errno == EINTR) {
                        this->m_eintr = true;
                    }
                    std::cerr << print("SSL_read", count) << std::endl;
                    return traits_type::eof();
                }                
                else if(count == 0) {
                    return traits_type::eof();
                }
                else {
                    char_type* end = this->eback()+count;
                    this->setg(this->eback(), this->eback(), end);
                    
                    return traits_type::to_int_type(*this->gptr());
                }
            }

            virtual int_type sync() {
                // OpenSSL writes to the socket itself, so MSG_NOSIGNAL cannot
                // reach it; on a platform without SO_NOSIGPIPE the guard is the
                // only thing standing between a dead peer and the process.
                sigpipe_guard guard;

                if(m_delay)
                    return Base::sync();

                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_tlsbuf::sync()"<<std::endl;

                int sofar = 0;
                int total = this->pptr() - this->pbase();
                int diff;
                int count;
                char_type* current = this->pbase();
                
                while( (diff=(total-sofar)) > 0 ) {
                    this->m_eintr = false;
                    count = SSL_write(m_ssl, current, diff);

                    if(count == -1) {
                        if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                            std::cerr << print("SSL_write", count) <<std::endl;

                        if(errno == EINTR) {
                            this->m_eintr = true;
                        }
                        return traits_type::eof();
                    }
                    sofar += count;
                    current += count;
                }
                
                this->setp(this->pbase(), this->pbase()+BUF_SIZE);
                return 0;                
            }

            virtual void close() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_tlsbuf::close()"<<std::endl;
                if(m_ssl != 0) {
                    SSL_shutdown(m_ssl);
                    SSL_free(m_ssl);
                    SSL_CTX_free(m_ctx);
                    m_ssl = 0;
                    m_ctx = 0;
                }
                Base::close();
            }

            void start() {
                m_delay = false;
                open_ssl();
            }

        protected:

            std::string print(const std::string& ctx, int err) {
                std::ostringstream o;
                o << ctx << " failed: ";
                
                int e = SSL_get_error(m_ssl, err);
                switch(e) {
                case SSL_ERROR_NONE:
                    o << "The TLS/SSL I/O operation completed.";
                    break;
                case SSL_ERROR_ZERO_RETURN:
                    o << "The TLS/SSL connection has been closed.";
                    break;
                case SSL_ERROR_WANT_READ:
                case SSL_ERROR_WANT_WRITE:
                    o << "The operation did not complete; the same TLS/SSL I/O function should be called again later.";
                    break;
                case SSL_ERROR_WANT_CONNECT:
                case SSL_ERROR_WANT_ACCEPT:
                    o << "The underlying BIO was not connected yet to the peer and the call would block in connect()/accept(). ";
                    break;
                case SSL_ERROR_WANT_X509_LOOKUP:
                    o << "The operation did not complete because an application callback set by SSL_CTX_set_client_cert_cb() has asked to be called again.";
                    break;
                case SSL_ERROR_SYSCALL:
                    o << "Some I/O error occurred.";
                    break;
                case SSL_ERROR_SSL:
                    o << "A failure in the SSL library occurred, usually a protocol error.";
                    break;
                }                

                o << std::endl;

                unsigned long r;
                const size_t N = 128;
                char buf[N];
                while( (r = ERR_get_error()) > 0 ) {
                    ERR_error_string_n(r, buf, N);
                    o << buf << std::endl;
                }

                return o.str();
            }

            void throw_if(const std::string& ctx, int err) {
                if(err <= 0) 
                    throw typename Base::exception(this->print(ctx, err));
            }

            void open_ssl() {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_tlsbuf::open_ssl()"<<std::endl;

                // OpenSSL 1.1 initializes itself on first use: SSL_library_init
                // and SSL_load_error_strings became no-ops there and are gone
                // in 3.0, along with the hand-rolled once-guard they needed.

                // TLS_client_method, always.  It was a constructor parameter,
                // and TLS_client_method was the only thing any caller ever
                // passed -- the older spellings it existed to allow are
                // SSLv2 and SSLv3, both removed from OpenSSL.
                m_ctx = SSL_CTX_new(TLS_client_method());
                if(m_ctx == 0) {
                    std::cerr <<"exception in jlib::sys::sslstream::open_ssl()"<<std::endl;
                    throw typename Base::exception("error calling SSL_CTX_new()");
                }

                // SSL_OP_NO_SSLv2 has been a no-op since 1.1 and SSLv3 is long
                // dead; state the floor directly instead.
                SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);

                SSL_CTX_set_verify(m_ctx, SSL_VERIFY_PEER, nullptr);

                // This used to hardcode /etc/ssl/certs, which does not exist on
                // macOS -- so with SSL_VERIFY_PEER set, every connection there
                // failed to validate.  Use the locations OpenSSL was built
                // with, still overridable via SSL_CERT_FILE and SSL_CERT_DIR.
                // NB: not print() here -- that reports via SSL_get_error(m_ssl),
                // and m_ssl does not exist until SSL_new below.
                if(!SSL_CTX_set_default_verify_paths(m_ctx)) {
                    throw typename Base::exception("error calling SSL_CTX_set_default_verify_paths()");
                }

                m_ssl = SSL_new(m_ctx);
                if(m_ssl == 0) {
                    std::cerr <<"exception in jlib::sys::sslstream::open_ssl()"<<std::endl;
                    throw typename Base::exception("error calling SSL_new()");
                }

                // Check that the certificate actually belongs to the host we
                // asked for.  Verifying the chain without this accepts any
                // valid certificate from any server the trust store covers,
                // which is the hole TODO.md meant by "verify certs".  Sending
                // SNI as well, so name-based virtual hosts serve the right one.
                // m_verify_host, not this->m_host: through a proxy the base
                // is connected to the proxy, and the certificate has to belong
                // to the server that was asked for.
                if(!m_verify_host.empty()) {
                    SSL_set_hostflags(m_ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
                    if(!SSL_set1_host(m_ssl, m_verify_host.c_str())) {
                        throw typename Base::exception(this->print("SSL_set1_host", 0));
                    }
                    SSL_set_tlsext_host_name(m_ssl, m_verify_host.c_str());
                }

                throw_if("SSL_set_fd", SSL_set_fd(m_ssl, this->m_sock));
                throw_if("SSL_connect", SSL_connect(m_ssl));
            }
            
            SSL_CTX* m_ctx;
            SSL* m_ssl;
            std::string m_verify_host;
            bool m_delay;
        };

        /** TLS straight over a socket. */
        template<typename charT, typename traitT = std::char_traits<charT> >
        using basic_sslbuf = basic_tlsbuf<basic_socketbuf<charT,traitT>, charT, traitT>;
        
        /**
         * A TLS connection, optionally upgraded in place.
         *
         * delay = false handshakes immediately, which is what imaps:// and
         * pop3s:// want.  delay = true connects in the clear and waits for
         * start(), which is what STARTTLS and STLS want.
         *
         * basic_sslstream was a separate class that did the first of those and
         * nothing else.  It is an alias now: "ssl" never meant the SSL
         * protocol here -- both classes have always used TLS_client_method --
         * it meant "handshake at once", and one flag says that more clearly
         * than two class names did.
         */
        template<typename charT, typename traitT=std::char_traits<charT> >
        class basic_tlsstream : public basic_socketstream<charT,traitT> {
        public:
            basic_tlsstream() 
                : basic_socketstream<charT,traitT>()
            {}

            basic_tlsstream(const std::string& host, unsigned int port, bool delay = false) 
                : basic_socketstream<charT,traitT>()
            {
                if(getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_tlsstream::basic_tlsstream(" << host << ", " << port << ", " << std::boolalpha << delay << ")"<<std::endl;
                this->m_buf = new basic_sslbuf<charT,traitT>(host, delay, host, port);
                this->init(this->m_buf);
            }
            
            void open(const std::string& host, unsigned int port, bool delay = false) {
                this->m_buf = new basic_sslbuf<charT,traitT>(host, delay, host, port);
                this->init(this->m_buf);
            }

            /** Handshake now, on a stream opened with delay = true. */
            void start() {
                dynamic_cast< basic_sslbuf<charT,traitT>* >(this->m_buf)->start();
            }

        };

        template<typename charT, typename traitT = std::char_traits<charT> >
        using basic_sslstream = basic_tlsstream<charT,traitT>;

        typedef basic_tlsstream< char, std::char_traits<char> > tlsstream;

        /** The older name for a tlsstream that handshakes at once. */
        typedef tlsstream sslstream;
       
    }
}


#endif // JLIB_SYS_SSLSTREAM_HH
