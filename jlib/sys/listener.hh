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

#ifndef JLIB_SYS_LISTENER_HH
#define JLIB_SYS_LISTENER_HH

#include <jlib/sys/socketstream.hh>

#include <memory>
#include <string>

namespace jlib {
namespace sys {

/**
 * The accepting end of a socket.
 *
 * jlib has been a client for twenty-five years: there was no bind, no listen
 * and no accept anywhere in it, and basic_socketbuf had exactly one
 * constructor, which connected.  Two things want the other end.  An OAuth2
 * authorization-code flow has to receive the redirect the browser makes to
 * http://127.0.0.1:<port>/, and the tests that need a server currently
 * hand-roll socket/bind/listen/getsockname inline -- sys_tls_sigpipe_test did,
 * and it was the model for this.
 *
 * Deliberately small.  It binds one address, accepts connections one at a time,
 * and hands each one back as a stream; it is not a server and does not want to
 * become one.
 *
 * The default address is loopback, not INADDR_ANY.  A receiver for a browser
 * redirect that binds every interface is reachable from the network, and what
 * arrives on it is an authorization code.
 */
/**
 * Who connected.
 *
 * ::accept(m_sock, 0, 0) discarded this for the whole of the listener's first
 * life, because nothing wanted it.  A server does: a handler that cannot tell a
 * loopback client from a stranger cannot make the one decision a loopback
 * receiver exists to make.
 */
struct peer {
    /**
     * Numeric, from getnameinfo(NI_NUMERICHOST).
     *
     * Never a name.  A reverse lookup is a stranger's DNS deciding what a log
     * line says, it costs a round trip on the accept path, and the answer is
     * not authenticated by anything.
     */
    std::string address;

    unsigned short port = 0;

    /** 127.0.0.0/8, ::1, or an IPv4-mapped loopback address. */
    bool loopback() const;
};

class listener {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = "listener exception: " + msg;
        }
        virtual ~exception() {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }
    protected:
        std::string m_msg;
    };

    /**
     * Bind and listen.
     *
     * @param port     the port, or 0 to let the kernel pick one -- port()
     *                 then says which, which is what a test and an OAuth
     *                 redirect URI both need.
     * @param host     the address to bind; loopback by default.  Resolved
     *                 with getaddrinfo, so "127.0.0.1", "::1" and "localhost"
     *                 all work, and "" means every interface -- say it out
     *                 loud if that is what you want, and expect it not to
     *                 work: a host with the macOS application firewall set to
     *                 block incoming connections, or any equivalent, filters
     *                 exactly that and leaves loopback alone.
     * @param backlog  listen(2)'s backlog.
     */
    explicit listener(unsigned short port = 0,
                      const std::string& host = "127.0.0.1",
                      int backlog = 8);

    ~listener();

    listener(const listener&) = delete;
    listener& operator=(const listener&) = delete;

    listener(listener&& other) noexcept;
    listener& operator=(listener&& other) noexcept;

    /** The port actually bound, which is the interesting one when 0 was asked for. */
    unsigned short port() const { return m_port; }

    int get_socket() const { return m_sock; }

    /**
     * Wait for one connection and return its descriptor.
     *
     * @param timeout  seconds to wait; zero waits forever.
     * @return the descriptor, or -1 if the timeout ran out.  A timeout is not
     *         an error -- a caller polling for a browser redirect while the
     *         user is still typing their password expects it -- so it is a
     *         return value and everything else throws.
     *
     * The caller owns the descriptor and must close it, or hand it to
     * accept_stream()'s buffer instead.
     */
    int accept(double timeout = 0);

    /** As accept(), and says who it was. */
    int accept(peer& from, double timeout = 0);

    /** accept(), wrapped in a stream.  Null if the timeout ran out. */
    std::unique_ptr<socketstream> accept_stream(double timeout = 0);

    std::unique_ptr<socketstream> accept_stream(peer& from, double timeout = 0);

    void close();

    /**
     * Take the listening socket out of blocking mode.
     *
     * For a caller that polls several descriptors at once and cannot let
     * accept() block on one of them -- sys::server does.  With it set, accept()
     * returns -1 rather than waiting when the poll and the accept disagree,
     * which happens when a client sends an RST between the two.
     *
     * It does not follow the connection: an accepted descriptor is always put
     * back into blocking mode, because on BSD and macOS it would otherwise
     * inherit this and every read a handler made would fail with EAGAIN.
     */
    void set_blocking(bool blocking);

private:
    int accept_into(peer* from, double timeout);

    int m_sock = -1;
    unsigned short m_port = 0;
};

}
}

#endif // JLIB_SYS_LISTENER_HH
