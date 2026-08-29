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

#ifndef JLIB_SYS_SOCKETSTREAM_HH
#define JLIB_SYS_SOCKETSTREAM_HH

#include <iostream>
#include <sstream>
#include <exception>
#include <string>


#include <cstring>
#include <cstdlib>

#include <jlib/sys/sys.hh>

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

namespace jlib {
    namespace sys {

        /**
         * Tag for the constructor that takes an already-connected descriptor.
         *
         * A tag rather than an overload because the fd is an int and so is a
         * port, and socketbuf(host, 5) meaning two different things depending
         * on the type of the first argument is the sort of thing that compiles
         * and then does not work.
         */
        struct adopt_t { explicit adopt_t() = default; };
        inline constexpr adopt_t adopt{};

        template< typename charT, typename traitT = std::char_traits<charT> >
        class basic_socketbuf : public std::basic_streambuf<charT,traitT> {
        public:
            class exception : public std::exception {
            public:
                exception(const std::string& msg = "") {
                    m_msg = "socket exception: "+msg;
                }
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            typedef charT 					            char_type;
            typedef traitT 					            traits_type;
            typedef typename traits_type::int_type 		int_type;
            typedef typename traits_type::pos_type 		pos_type;
            typedef typename traits_type::off_type 		off_type;
            
            static const unsigned int BUF_SIZE = 1024;

            /**
             * Connect to host:port.
             *
             * @param timeout  seconds to allow the connect; negative takes
             *                 sys::get_default_connect_timeout(), zero waits
             *                 forever.
             */
            basic_socketbuf(const std::string& host, unsigned int port, double timeout = -1) {
                init_buffers();

                // A constructor that throws gets no destructor, so the two
                // buffers just allocated would be lost -- and a failing
                // connect is the ordinary case for a mail client retrying a
                // server that is down, so it leaks once a minute rather than
                // once ever.
                try {
                    open_socket(host, port, timeout);
                }
                catch(...) {
                    free_buffers();
                    throw;
                }
            }

            /**
             * Take over a descriptor that is already connected.
             *
             * This is what an accepted connection needs, and until it existed
             * there was no way to get one into a stream at all: the only
             * constructor connected, so jlib could be a client and nothing
             * else.  sys::listener hands its fd in here, and so does the OAuth
             * redirect receiver.
             *
             * The buf owns the descriptor from here and closes it.  host and
             * port are what to *call* the peer -- they are used for messages,
             * and by basic_tlsbuf for the name it verifies against -- not for
             * anything to connect to.
             */
            basic_socketbuf(adopt_t, int fd, const std::string& host = "",
                            unsigned int port = 0, double timeout = -1) {
                init_buffers();
                m_host = host;
                m_port = port;
                m_sock = fd;

                // Before configure(), which is the only place it can be got in
                // front of: configure() calls apply_timeout() unconditionally,
                // so a SO_RCVTIMEO set on the descriptor before adopting it is
                // overwritten with the library default -- which is forever.
                // That is why this parameter exists rather than the caller
                // setting the option itself: a server has to bound the TLS
                // handshake, and the handshake happens inside the constructor
                // that adopts the descriptor.
                m_io_timeout = timeout;

                try {
                    configure(m_sock);
                }
                catch(...) {
                    close();
                    free_buffers();
                    throw;
                }
            }

            virtual ~basic_socketbuf() {
                if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_socketbuf::~basic_socketbuf()"<<std::endl;
                close();
                free_buffers();
            }

            virtual int_type underflow() {
                if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_socketbuf::underflow()"<<std::endl;

                m_eintr = false;
                m_timeout = false;
                int count = ::read(m_sock, this->eback(), BUF_SIZE);
                
                if(count < 0) {
                    if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                        std::cerr <<"exception in read(2) at jlib::sys::socketstream::underflow()"<<std::endl;
                    //this->setstate(std::ios_base::badbit);
                    if(errno == EINTR) {
                        m_eintr = true;
                    }
                    // SO_RCVTIMEO expiring looks exactly like end of stream
                    // from out here -- eof() is all a streambuf can return --
                    // so record it, or a caller cannot tell "the server closed"
                    // from "the server is slow".
                    if(errno == EAGAIN || errno == EWOULDBLOCK) {
                        m_timeout = true;
                    }
                    return traits_type::eof();
                    //throw exception("error reading");
                }                
                else if(count == 0) {
                    if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                        std::cerr <<"eof in read(2) at jlib::sys::socketstream::underflow()"<<std::endl;
                    return traits_type::eof();
                }
                else {
                    char_type* end = this->eback()+count;
                    this->setg(this->eback(), this->eback(), end);
                    
                    return traits_type::to_int_type(*this->gptr());
                }
            }

            virtual int_type overflow(int_type c=traits_type::eof()) {
                if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_socketbuf::overflow("<<c<<")"<<std::endl;
                if(this->pptr() >= this->epptr()) {
                    if(sync() == traits_type::eof()) {
                        return traits_type::eof();
                    }
                }
                
                *this->pptr() = c;
                this->pbump(1);
                return c;
            }

            virtual int_type sync() {
                if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_socketbuf::sync()"<<std::endl;
                // Where SO_NOSIGPIPE exists this is a no-op and the socket
                // option has already dealt with it; where it does not, this is
                // what keeps a dead peer from killing us.
                sigpipe_guard guard;

                int sofar = 0;
                int total = this->pptr() - this->pbase();
                int diff;
                int count;
                char_type* current = this->pbase();
                
                while( (diff=(total-sofar)) > 0 ) {
                    m_eintr = false;
                    m_timeout = false;
                    count = write(m_sock, current, diff);

                    if(count == -1) {
                        if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                            std::cerr <<"exception in jlib::sys::socketstream::sync()"<<std::endl;
                        if(errno == EINTR) {
                            m_eintr = true;
                        }
                        if(errno == EAGAIN || errno == EWOULDBLOCK) {
                            m_timeout = true;
                        }
                        // EPIPE arrives here now instead of as a signal.
                        return traits_type::eof();
                    }
                    sofar += count;
                    current += count;
                }
                
                this->setp(this->pbase(), this->pbase()+BUF_SIZE);
                return 0;                
            }

            virtual void close() {
                if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                    std::cerr << "basic_socketbuf::close()"<<std::endl;
                if(m_sock != -1) {
                    ::close(m_sock);
                    m_sock = -1;
                }
            }

            bool interrupted() { return m_eintr; }

            /** Whether the last read or write gave up on a timeout. */
            bool timed_out() const { return m_timeout; }

            /**
             * How long a read or write may block, in seconds.  Zero is forever.
             *
             * Applies from the next call; anything already blocked in read(2)
             * stays there.  Takes effect through SO_RCVTIMEO and SO_SNDTIMEO,
             * so a timeout shows up as a short read or an EAGAIN, not as a
             * signal, and timed_out() says which it was.
             */
            void set_timeout(double seconds) {
                m_io_timeout = seconds < 0 ? 0 : seconds;
                if(m_sock != -1)
                    apply_timeout(m_sock, m_io_timeout);
            }

            double get_timeout() const { return m_io_timeout; }

            int get_socket() { return m_sock; }
            
        protected:
            void init_buffers() {
                char_type* tmp;

                tmp = new char_type[BUF_SIZE];
                this->setg(tmp,tmp,tmp);

                tmp = new char_type[BUF_SIZE];
                this->setp(tmp,tmp+BUF_SIZE);

                //_M_mode = (std::ios_base::in | std::ios_base::out);
            }

            void free_buffers() {
                delete [] this->eback();
                delete [] this->pbase();
                this->setg(0,0,0);
                this->setp(0,0);
            }

            static void apply_timeout(int fd, double seconds) {
                struct timeval tv;

                tv.tv_sec = static_cast<time_t>(seconds);
                tv.tv_usec = static_cast<suseconds_t>((seconds - tv.tv_sec) * 1e6);

                // A zero timeval is how the kernel spells "no timeout", so
                // seconds == 0 turns it off rather than making every call time
                // out at once.
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            }

            /** Everything a connected descriptor needs, however it was got. */
            void configure(int fd) {
                // Before anything is written: a write to a peer that has gone
                // away raises SIGPIPE, whose default action is to kill the
                // process outright, so the error checking elsewhere never runs.
                nosigpipe(fd);

                if(m_io_timeout < 0)
                    m_io_timeout = get_default_io_timeout();
                apply_timeout(fd, m_io_timeout);

                if(fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
                    if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                        std::cerr <<"throwing exception from jlib::sys::socketstream"<<std::endl
                                  <<"error calling fcntl(sock, F_SETFD, FD_CLOEXEC)"<<std::endl;
                    throw exception("error calling fcntl(sock, F_SETFD, FD_CLOEXEC)");
                }
            }

            /**
             * connect(2) with a deadline.
             *
             * Returns 0 on success and an errno on failure, with ETIMEDOUT for
             * running out of time.  A blocking connect cannot be given a
             * deadline at all, so the descriptor goes non-blocking for the
             * duration and is put back afterwards -- the caller gets an
             * ordinary blocking socket either way.
             */
            int connect_within(int fd, const struct sockaddr* sa, socklen_t len, double seconds) {
                const int flags = fcntl(fd, F_GETFL, 0);

                if(flags == -1)
                    return errno;

                if(seconds > 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
                    return errno;

                int err = 0;

                if(::connect(fd, sa, len) == 0) {
                    err = 0;
                }
                else if(errno == EINTR) {
                    m_eintr = true;
                    err = EINTR;
                }
                else if(seconds > 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK)) {
                    struct pollfd p;

                    p.fd = fd;
                    p.events = POLLOUT;
                    p.revents = 0;

                    const int ms = static_cast<int>(seconds * 1000);
                    const int r = ::poll(&p, 1, ms);

                    if(r == 0) {
                        err = ETIMEDOUT;
                    }
                    else if(r < 0) {
                        err = errno;
                        if(err == EINTR)
                            m_eintr = true;
                    }
                    else {
                        // POLLOUT says the connect finished, not that it
                        // succeeded -- a refused connection is also writable.
                        // SO_ERROR is the only thing that says which.
                        int so = 0;
                        socklen_t solen = sizeof(so);

                        if(::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &solen) < 0)
                            err = errno;
                        else
                            err = so;
                    }
                }
                else {
                    err = errno;
                }

                if(seconds > 0)
                    fcntl(fd, F_SETFL, flags);

                return err;
            }

            void open_socket(const std::string& host, unsigned int port, double timeout = -1) {
                m_host = host;
                m_port = port;
                m_sock = -1;
                m_eintr = false;

                const double allow = timeout < 0 ? get_default_connect_timeout() : timeout;

                // getaddrinfo, not gethostbyname.  Two reasons, both of which
                // had bitten: gethostbyname is IPv4-only, so a name with only
                // an AAAA record -- which is now an ordinary thing for a mail
                // or token endpoint to be -- simply did not resolve; and it
                // returns a pointer to static storage, so two of the threaded
                // AS* mailboxes resolving at once overwrote each other's answer
                // and one of them connected somewhere it had never asked for.
                struct addrinfo hints;
                std::memset(&hints, 0, sizeof(hints));
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;

                std::ostringstream o; o << port;
                const std::string service = o.str();

                struct addrinfo* res = 0;
                const int gai = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);

                if(gai != 0 || res == 0) {
                    if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                        std::cerr <<"throwing exception from jlib::sys::socketstream::open_socket()"<<std::endl
                                  << "error in getaddrinfo("<<host<<")"<<std::endl;
                    throw exception("error resolving " + host + ": " +
                                    ::gai_strerror(gai));
                }

                // Every address the name has, in the order the resolver gave
                // them, which is the order RFC 6724 wants them tried in.  One
                // AAAA that happens to be unreachable is no longer the whole
                // connection.
                int last = 0;

                for(struct addrinfo* ai = res; ai != 0 && m_sock == -1; ai = ai->ai_next) {
                    const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

                    if(fd < 0) {
                        last = errno;
                        continue;
                    }

                    last = connect_within(fd, ai->ai_addr, ai->ai_addrlen, allow);

                    if(last == 0)
                        m_sock = fd;
                    else
                        ::close(fd);
                }

                ::freeaddrinfo(res);

                if(m_sock == -1) {
                    if(std::getenv("JLIB_SYS_SOCKET_DEBUG"))
                        std::cerr <<"throwing exception from jlib::sys::socketstream::open_socket()"<<std::endl
                                  <<"error in connect()"<<std::endl;
                    throw exception("error connecting to " + host + ":" + service + ": " +
                                    std::strerror(last));
                }

                configure(m_sock);
            }

            std::string m_host;
            unsigned int m_port = 0;
            int m_sock = -1;
            bool m_eintr = false;
            bool m_timeout = false;

            // Negative until configure() resolves it against the library
            // default, so a set_timeout() before the connect is not overwritten
            // by one after it.
            double m_io_timeout = -1;
        };
        
        template<typename charT, typename traitT=std::char_traits<charT> >
        class basic_socketstream : public std::basic_iostream<charT,traitT> {
        public:
            basic_socketstream() 
                : std::basic_iostream<charT,traitT>(NULL)
            {
                m_buf = 0;
                //exceptions(std::ios_base::badbit);
            }

            basic_socketstream(const std::string& host, unsigned int port, double timeout = -1)
                : std::basic_iostream<charT,traitT>(NULL)
            {
                m_buf = 0;
                //exceptions(std::ios_base::badbit);
                m_buf=new basic_socketbuf<charT,traitT>(host,port,timeout);
                this->init(m_buf);
            }

            /** Take over an already-connected descriptor; see basic_socketbuf. */
            basic_socketstream(adopt_t, int fd, const std::string& host = "",
                               unsigned int port = 0, double timeout = -1)
                : std::basic_iostream<charT,traitT>(NULL)
            {
                m_buf = 0;
                m_buf=new basic_socketbuf<charT,traitT>(adopt,fd,host,port,timeout);
                this->init(m_buf);
            }

            virtual ~basic_socketstream() {
                if(m_buf != 0)
                    delete m_buf;
            }
            
            void open(const std::string& host, unsigned int port, double timeout = -1) {
                if(m_buf != 0)
                    delete m_buf;
                m_buf=new basic_socketbuf<charT,traitT>(host,port,timeout);
                this->init(m_buf);
            }

            /** As the adopting constructor.  There was no open() for one. */
            void open(adopt_t, int fd, const std::string& host = "",
                      unsigned int port = 0, double timeout = -1) {
                if(m_buf != 0)
                    delete m_buf;
                m_buf=new basic_socketbuf<charT,traitT>(adopt,fd,host,port,timeout);
                this->init(m_buf);
            }

            // Every one of these dereferenced m_buf without checking it, so
            // any of them on a default-constructed socketstream -- which is a
            // perfectly ordinary thing to have, since open() exists -- was a
            // null dereference rather than an error.
            void close() {
                if(m_buf != 0) m_buf->close();
            }

            bool interrupted() { return m_buf != 0 && m_buf->interrupted(); }

            /** Whether the last read or write gave up on a timeout. */
            bool timed_out() const { return m_buf != 0 && m_buf->timed_out(); }

            /** Seconds a read or write may block; zero is forever. */
            void set_timeout(double seconds) {
                if(m_buf != 0) m_buf->set_timeout(seconds);
            }

            double get_timeout() const { return m_buf != 0 ? m_buf->get_timeout() : 0; }

            /** -1 when there is no connection, as an unopened descriptor is. */
            int get_socket() { return m_buf != 0 ? m_buf->get_socket() : -1; }
            
        protected:
            basic_socketbuf<charT,traitT>* m_buf;
        };
    
        typedef basic_socketstream< char, std::char_traits<char> > socketstream;
        
    }
}


#endif // JLIB_SYS_SOCKETSTREAM_HH
