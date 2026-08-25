/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
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

#include <jlib/sys/listener.hh>
#include <jlib/sys/sys.hh>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace jlib {
namespace sys {

namespace {

    unsigned short port_of(const struct sockaddr_storage& ss) {
        if(ss.ss_family == AF_INET6)
            return ntohs(reinterpret_cast<const struct sockaddr_in6*>(&ss)->sin6_port);

        return ntohs(reinterpret_cast<const struct sockaddr_in*>(&ss)->sin_port);
    }

}

listener::listener(unsigned short port, const std::string& host, int backlog) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    std::ostringstream o; o << port;
    const std::string service = o.str();

    struct addrinfo* res = 0;
    const int gai = ::getaddrinfo(host.empty() ? 0 : host.c_str(),
                                  service.c_str(), &hints, &res);

    if(gai != 0 || res == 0)
        throw exception("error resolving " + (host.empty() ? std::string("*") : host) +
                        ": " + ::gai_strerror(gai));

    int last = 0;

    for(struct addrinfo* ai = res; ai != 0 && m_sock == -1; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

        if(fd < 0) {
            last = errno;
            continue;
        }

        // Otherwise the port sits in TIME_WAIT for a couple of minutes after
        // the process exits, and a test that runs twice in a row fails the
        // second time for no reason the test can see.
        int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        if(::bind(fd, ai->ai_addr, ai->ai_addrlen) < 0 || ::listen(fd, backlog) < 0) {
            last = errno;
            ::close(fd);
            continue;
        }

        m_sock = fd;
    }

    ::freeaddrinfo(res);

    if(m_sock == -1)
        throw exception("error listening on " + host + ":" + service + ": " +
                        std::strerror(last));

    // Ask the socket what it got, rather than believing what was asked for:
    // with port 0 the kernel chose, and the caller needs the answer to put in
    // a redirect URI or to connect a test client to.
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);

    if(::getsockname(m_sock, reinterpret_cast<struct sockaddr*>(&ss), &len) < 0) {
        const int e = errno;
        close();
        throw exception(std::string("error in getsockname(): ") + std::strerror(e));
    }

    m_port = port_of(ss);

    ::fcntl(m_sock, F_SETFD, FD_CLOEXEC);
}

listener::~listener() {
    close();
}

listener::listener(listener&& other) noexcept
    : m_sock(other.m_sock),
      m_port(other.m_port)
{
    other.m_sock = -1;
    other.m_port = 0;
}

listener& listener::operator=(listener&& other) noexcept {
    if(this != &other) {
        close();
        m_sock = other.m_sock;
        m_port = other.m_port;
        other.m_sock = -1;
        other.m_port = 0;
    }

    return *this;
}

int listener::accept(double timeout) {
    if(m_sock == -1)
        throw exception("accept() on a closed listener");

    if(timeout > 0) {
        struct pollfd p;

        p.fd = m_sock;
        p.events = POLLIN;
        p.revents = 0;

        int r;

        // poll(2) coming back EINTR is not the timeout expiring, and treating
        // it as one would make a stray SIGCHLD look like "nobody connected".
        while((r = ::poll(&p, 1, static_cast<int>(timeout * 1000))) < 0 && errno == EINTR)
            ;

        if(r == 0)
            return -1;

        if(r < 0)
            throw exception(std::string("error in poll(): ") + std::strerror(errno));
    }

    int fd;

    while((fd = ::accept(m_sock, 0, 0)) < 0 && errno == EINTR)
        ;

    if(fd < 0)
        throw exception(std::string("error in accept(): ") + std::strerror(errno));

    return fd;
}

std::unique_ptr<socketstream> listener::accept_stream(double timeout) {
    const int fd = accept(timeout);

    if(fd < 0)
        return std::unique_ptr<socketstream>();

    try {
        return std::make_unique<socketstream>(adopt, fd);
    }
    catch(...) {
        ::close(fd);
        throw;
    }
}

void listener::close() {
    if(m_sock != -1) {
        ::close(m_sock);
        m_sock = -1;
    }
}

}
}
