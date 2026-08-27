/* -*- mode: C++ c-basic-offset: 4 -*-
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
 */

#ifndef JLIB_TESTS_HTTPSERVER_HH
#define JLIB_TESTS_HTTPSERVER_HH

// An HTTP server that answers whatever it is told to, in this process.
//
// For the cases a real server will not produce on request: a chunked body
// whose chunk data contains a line that looks like a chunk header, a response
// carrying both Content-Length and Transfer-Encoding, a Location: pointing
// somewhere it must not be followed to.  Those are the shapes that break a
// client, and no amount of asking nginx nicely will get them.
//
// It also records every request it was sent, which is how a test asserts what
// the *client* did -- that the query string reached the request-target, that
// Authorization came off across an origin change, that a POST body was not
// re-sent to a redirect.
//
// It speaks TLS when handed a certificate, which is the only way jlib gets an
// HTTPS test that runs on a developer's machine: net_http_live_test needs an
// nginx and there is not one outside the build container.  Server-side TLS is
// raw OpenSSL here rather than jlib's own -- basic_tlsbuf calls SSL_connect and
// there is no server-side counterpart in the library, which is a gap this test
// works around rather than one it fixes.
//
// It is not a substitute for a real server and net_http_live_test is the other
// half: a server nobody wrote sends responses nobody thought of.  This one
// sends exactly what somebody thought of, which is the point of it and also
// its whole limitation.

#include <jlib/sys/listener.hh>
#include <jlib/sys/socketstream.hh>

#include <jlib/util/http.hh>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <functional>
#include <istream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace httpserver {

/** One request as it arrived. */
struct request {
    std::string head;   ///< the request line and fields, CRLFs and all
    std::string body;
};

class server {
public:
    /** Given the request, return the raw octets to send back. */
    typedef std::function<std::string(const request&)> handler;

    /**
     * @param h     what to answer with
     * @param cert  a PEM certificate, or "" for a plaintext server
     * @param key   its private key
     *
     * With a certificate this speaks TLS, and the client is expected to
     * verify it for real -- point SSL_CERT_FILE at the same file and connect
     * to a name the certificate covers.  Nothing here weakens verification;
     * the handshake succeeds because the certificate is genuinely trusted for
     * the run, which is the pattern mailserver.hh and sys_tls_sigpipe_test
     * both use.
     */
    explicit server(handler h,
                    const std::string& cert = std::string(),
                    const std::string& key = std::string())
        : m_listener(),
          m_handler(std::move(h))
    {
        if(!cert.empty()) {
            m_ctx = SSL_CTX_new(TLS_server_method());

            if(!m_ctx ||
               SSL_CTX_use_certificate_file(m_ctx, cert.c_str(), SSL_FILETYPE_PEM) != 1 ||
               SSL_CTX_use_PrivateKey_file(m_ctx, key.c_str(), SSL_FILETYPE_PEM) != 1) {
                if(m_ctx) SSL_CTX_free(m_ctx);

                m_ctx = 0;

                throw std::runtime_error("could not set up a TLS test server");
            }
        }

        m_thread = std::thread([this] { run(); });
    }

    ~server() {
        stop();

        if(m_ctx) SSL_CTX_free(m_ctx);
    }

    server(const server&) = delete;
    server& operator=(const server&) = delete;

    unsigned short port() const { return m_listener.port(); }

    bool tls() const { return m_ctx != 0; }

    /**
     * Where to reach it.
     *
     * The TLS form says "localhost" rather than 127.0.0.1, because that is the
     * name the generated certificate covers and jlib checks it with
     * SSL_set1_host.
     */
    std::string url(const std::string& path = "/") const {
        return (m_ctx ? "https://localhost:" : "http://127.0.0.1:") +
               std::to_string(m_listener.port()) + path;
    }

    /** Every request served so far. */
    std::vector<request> requests() const {
        std::lock_guard<std::mutex> lock(m_lock);

        return m_requests;
    }

    void stop() {
        m_stop = true;

        if(m_thread.joinable()) m_thread.join();

        m_listener.close();
    }

private:
    /**
     * One accepted connection, plain or TLS.
     *
     * A pair of read/write calls rather than a streambuf, because there is no
     * server-side one in jlib to reach for and writing one to serve a test
     * would be the tail wagging the dog.
     */
    class conn {
    public:
        conn(int fd, SSL_CTX* ctx) : m_fd(fd) {
            if(!ctx) return;

            m_ssl = SSL_new(ctx);

            if(!m_ssl || SSL_set_fd(m_ssl, fd) != 1 || SSL_accept(m_ssl) != 1) {
                if(m_ssl) { SSL_free(m_ssl); m_ssl = 0; }

                // Closed here, not left to the destructor: a constructor that
                // throws does not get one, and the descriptor would leak once
                // per failed handshake.
                ::close(m_fd);
                m_fd = -1;

                throw std::runtime_error("the TLS handshake failed");
            }
        }

        ~conn() {
            if(m_ssl) {
                SSL_shutdown(m_ssl);
                SSL_free(m_ssl);
            }

            if(m_fd >= 0) ::close(m_fd);
        }

        conn(const conn&) = delete;
        conn& operator=(const conn&) = delete;

        /** Bytes read, 0 at end of stream. */
        std::size_t read(char* buf, std::size_t n) {
            const int got = m_ssl ? SSL_read(m_ssl, buf, static_cast<int>(n))
                                  : static_cast<int>(::read(m_fd, buf, n));

            return got > 0 ? static_cast<std::size_t>(got) : 0;
        }

        void write(const std::string& s) {
            std::size_t sent = 0;

            while(sent < s.size()) {
                const int n = m_ssl
                    ? SSL_write(m_ssl, s.data() + sent, static_cast<int>(s.size() - sent))
                    : static_cast<int>(::write(m_fd, s.data() + sent, s.size() - sent));

                if(n <= 0) return;

                sent += static_cast<std::size_t>(n);
            }
        }

    private:
        int m_fd = -1;
        SSL* m_ssl = 0;
    };

    /** Exactly n octets, or fewer if the peer stopped. */
    static std::string take(conn& c, std::size_t n) {
        std::string out;

        while(out.size() < n) {
            char buf[4096];
            const std::size_t want = std::min(sizeof buf, n - out.size());
            const std::size_t got = c.read(buf, want);

            if(!got) break;

            out.append(buf, got);
        }

        return out;
    }

    void run() {
        while(!m_stop) {
            int fd = -1;

            try {
                // A short deadline rather than a blocking accept, so that
                // stopping does not depend on one more client turning up.
                fd = m_listener.accept(0.2);
            }
            catch(std::exception&) {
                return;
            }

            if(fd < 0) continue;

            try {
                conn c(fd, m_ctx);

                request r;

                // A byte at a time to the blank line.  util::http::read_head()
                // does this over an istream and there is not one here; the
                // duplication is four lines and the alternative is a
                // server-side streambuf nothing else would use.
                while(r.head.size() < 4 ||
                      r.head.compare(r.head.size() - 4, 4, "\r\n\r\n") != 0) {
                    char one;

                    if(!c.read(&one, 1)) break;

                    r.head += one;

                    if(r.head.size() > 65536) break;
                }

                // A connection that carried no complete head is not a
                // request and must not be recorded as one.  read_head(), which
                // this replaced, threw on end of stream and so never got here;
                // the loop above just stops, and recording what it had made a
                // client that connected and thought better of it look exactly
                // like one that asked for something.
                if(r.head.size() < 4 ||
                   r.head.compare(r.head.size() - 4, 4, "\r\n\r\n") != 0) {
                    continue;
                }

                // Only what a test sends: a Content-Length body, or none.
                const std::string::size_type at = find_ci(r.head, "\r\ncontent-length:");

                if(at != std::string::npos) {
                    const std::string::size_type eol = r.head.find("\r\n", at + 2);
                    const std::string value = r.head.substr(at + 17, eol - at - 17);

                    r.body = take(c, std::stoul(value));
                }

                {
                    std::lock_guard<std::mutex> lock(m_lock);

                    m_requests.push_back(r);
                }

                // The client sends Connection: close and so does this; conn's
                // destructor closing is what ends a body with no framing
                // fields on it.
                c.write(m_handler(r));
            }
            catch(std::exception&) {
                // A client that hung up mid-request, or a handshake that did
                // not complete, are both cases some tests create on purpose.
                // conn closes the descriptor either way.
            }
        }
    }

    static std::string::size_type find_ci(const std::string& hay, const std::string& needle) {
        std::string lower;

        for(char c : hay) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        return lower.find(needle);
    }

    jlib::sys::listener m_listener;
    handler m_handler;
    SSL_CTX* m_ctx = 0;
    mutable std::mutex m_lock;
    std::vector<request> m_requests;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

/** A minimal well-formed response. */
inline std::string reply(int status, const std::string& reason,
                         const std::string& body = "",
                         const std::string& extra = "")
{
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" +
           "Content-Length: " + std::to_string(body.size()) + "\r\n" +
           extra +
           "\r\n" + body;
}

}

#endif // JLIB_TESTS_HTTPSERVER_HH
