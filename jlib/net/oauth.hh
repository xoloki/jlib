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

#ifndef JLIB_NET_OAUTH_HH
#define JLIB_NET_OAUTH_HH

#include <jlib/net/http.hh>

#include <cstddef>
#include <ctime>
#include <functional>
#include <memory>
#include <string>

namespace jlib {
namespace net {

/**
 * OAuth2, as much of it as reading mail needs.
 *
 * Gmail has wanted this since 2022 and Outlook.com has required it for personal
 * accounts since September 2024; a mail client that speaks only LOGIN and
 * AUTH PLAIN cannot reach either.
 *
 * ## What is here, and what is not
 *
 * Here: the refresh-token grant (RFC 6749 6), which turns a refresh token you
 * already hold into an access token, and XOAUTH2, which is how that access
 * token reaches an IMAP server.
 *
 * Here too, since the branch that added it: the authorization-code flow with
 * PKCE, which obtains that first refresh token -- authorize_url(),
 * redirect_receiver, exchange(), and authorize() tying the three together.
 *
 * A caveat that is not a bug and cannot be fixed here: neither Google nor
 * Microsoft will issue a client id without the user registering an
 * application with them first.  jlib can be complete and this can still not
 * work for somebody who has not done that.
 *
 * Not here: the device-code flow, OAUTHBEARER (RFC 7628), token introspection,
 * or anything that stores a credential on disk.  Where the refresh token lives
 * is the application's decision and jlib should not make it.
 *
 * ## Two things the interface has to get right, not the implementation
 *
 * **Rotation.**  RFC 6749 6 lets the token endpoint issue a *new* refresh token
 * with every refresh, and Microsoft always does, invalidating the old one at the
 * same moment.  An API that takes a refresh token and cannot hand a new one back
 * logs the user out after the first refresh and cannot be fixed without changing
 * every caller.  So token::refresh() is always the one to keep, whether or not
 * it changed, and session has a store callback that fires the instant a new one
 * arrives -- before the access token beside it is used for anything, because a
 * crash in between is the same lockout.
 *
 * **A client secret in a native application is not secret.**  RFC 8252 8.5 says
 * so plainly: it ships inside the binary and anyone who has the binary has it.
 * client::secret exists because Microsoft's older endpoints reject a request
 * without one, not because it protects anything, and nothing here is compiled
 * in.  A caller supplies their own registration.
 */
namespace oauth {

class error : public std::exception {
public:
    error(const std::string& msg = "") { m_msg = "oauth error: " + msg; }
    virtual ~error() {}
    virtual const char* what() const noexcept { return m_msg.c_str(); }
protected:
    std::string m_msg;
};

/**
 * The endpoint refused, and said why in a way a program can act on.
 *
 * RFC 6749 5.2.  The one that matters is "invalid_grant": the refresh token is
 * dead -- revoked, expired, or rotated out from under you -- and no amount of
 * retrying will help.  The user has to authorize again.  Anything else is worth
 * retrying.
 */
class denied : public error {
public:
    denied(const std::string& code, const std::string& description)
        : error("the token endpoint refused: " + code +
                (description.empty() ? "" : " (" + description + ")")),
          m_code(code),
          m_description(description)
    {}

    const std::string& code() const { return m_code; }
    const std::string& description() const { return m_description; }

    /** Nothing will fix this but authorizing again. */
    bool fatal() const { return m_code == "invalid_grant" || m_code == "invalid_client"; }

private:
    std::string m_code;
    std::string m_description;
};

/** Who we say we are, and where to say it. */
struct client {
    /** The provider's token endpoint.  Must be https unless allow_http. */
    std::string token_endpoint;

    /** Where a browser is sent to authorize.  Only the code grant needs it. */
    std::string authorize_endpoint;

    std::string id;

    /** Often empty; see the note above about what it is not. */
    std::string secret;

    /** Space-separated, and optional on a refresh (RFC 6749 6). */
    std::string scope;

    /**
     * Send a client credential to a plaintext endpoint.
     *
     * Off, and only a test should turn it on.  A token request over http://
     * puts the refresh token and the client secret on the wire in the clear,
     * and the access token comes back the same way.
     */
    bool allow_http = false;
};

/** What came back from the token endpoint. */
class token {
public:
    token() = default;

    const std::string& access() const { return m_access; }

    /**
     * The refresh token to keep.
     *
     * The new one when the endpoint issued one, and the one that was sent when
     * it did not.  Either way this is what to store: see the note on rotation.
     */
    const std::string& refresh() const { return m_refresh; }

    /** Whether the endpoint issued a new refresh token this time. */
    bool rotated() const { return m_rotated; }

    /** "Bearer", almost always. */
    const std::string& type() const { return m_type; }

    const std::string& scope() const { return m_scope; }

    /** When it stops being accepted, as a time_t.  0 if unknown. */
    std::time_t expires_at() const { return m_expires_at; }

    /**
     * Whether it is worth using.
     *
     * With a minute of slack, because the clock here and the clock there are
     * not the same clock and a token that expires between the check and the
     * command is a failure that looks like a wrong password.
     */
    bool expired(std::time_t now = 0) const;

    bool empty() const { return m_access.empty(); }

    friend token parse_token_response(const std::string& body,
                                      const std::string& sent_refresh,
                                      std::time_t now);

private:
    std::string m_access;
    std::string m_refresh;
    std::string m_type;
    std::string m_scope;
    std::time_t m_expires_at = 0;
    bool m_rotated = false;
};

/**
 * Read a token endpoint's reply.
 *
 * Exposed because it is the half worth testing without a server: every
 * provider's oddity -- expires_in as a string, a missing refresh_token, an
 * error body with a 200 status -- is a string this can be handed.
 *
 * @param sent_refresh what was sent, so refresh() has something to fall back on
 * @param now          for a test that needs a fixed clock; 0 means std::time
 *
 * @throw denied on an RFC 6749 5.2 error body, error on anything unreadable
 */
token parse_token_response(const std::string& body,
                           const std::string& sent_refresh,
                           std::time_t now = 0);

/**
 * Exchange a refresh token for an access token.  RFC 6749 6.
 *
 * @throw denied when the endpoint says no in the documented way, error
 *        otherwise.  A 400 with a JSON body is the *normal* failure and comes
 *        back as denied with its code; that is the whole reason the HTTP client
 *        hands back a non-2xx rather than throwing.
 */
token refresh(const client& c, const std::string& refresh_token,
              const http::options& o = http::options());

/**
 * An access token, kept fresh.
 *
 * Holds a refresh token, fetches an access token when there is none or the one
 * it has is stale, and hands the same one out until it goes stale.  The cache
 * is an optimisation and never the truth: if the server rejects a token, say so
 * with rejected() and the next access() fetches another, whatever the expiry
 * said.  Clocks disagree and tokens get revoked mid-session.
 *
 * Not thread-safe.  Run it on the thread that owns the connection, which is the
 * house pattern -- ASMailBox and ASImapBox do their work on a sys::ASServent.
 */
class session {
public:
    /** Called the instant a new refresh token arrives.  Persist it here. */
    typedef std::function<void(const token&)> store_fn;

    session(client c, std::string refresh_token, store_fn store = store_fn());

    /** A usable access token, fetching or refreshing as needed. */
    const std::string& access();

    /** The refresh token as it stands, which access() may have changed. */
    const std::string& refresh_token() const { return m_refresh; }

    /** The whole of the last reply. */
    const token& current() const { return m_token; }

    /**
     * The server did not accept the last access token.
     *
     * Drops it, so the next access() fetches another.  A NO from IMAP is
     * authoritative and the expiry this holds is only a guess.
     */
    void rejected() { m_token = token(); }

    const client& who() const { return m_client; }

private:
    client m_client;
    std::string m_refresh;
    store_fn m_store;
    token m_token;
};

/**
 * PKCE, RFC 7636: a secret and the hash a request carries in its place.
 *
 * The authorization code comes back over a loopback HTTP redirect, which any
 * other application on the machine can race for -- it is a plain TCP port on
 * localhost.  PKCE is what makes stealing the code useless: the exchange that
 * turns it into a token also demands the verifier, which never left this
 * process.
 *
 * **S256 only.**  RFC 7636 4.2 also defines "plain", where the challenge *is*
 * the verifier, and RFC 8252 8.1 says a native application must use S256.
 * Offering plain would only be useful to a server that cannot do SHA-256, and
 * a client that can be talked down to plain by a server's say-so has no
 * protection at all.
 */
class pkce {
public:
    /** A fresh verifier from the system's CSPRNG. */
    static pkce generate();

    /** Build from a verifier you already have; validates its shape. */
    explicit pkce(std::string verifier);

    /** RFC 7636 4.1: 43-128 characters of unreserved.  Never sent anywhere but
     *  the token request. */
    const std::string& verifier() const { return m_verifier; }

    /** base64url(SHA-256(verifier)), unpadded.  This is what goes in the URL. */
    const std::string& challenge() const { return m_challenge; }

    /** "S256". */
    static const char* method() { return "S256"; }

private:
    std::string m_verifier;
    std::string m_challenge;
};

/** Unguessable text from the system CSPRNG, base64url, for state and nonces. */
std::string random_token(std::size_t bytes = 32);

/**
 * What came back to the redirect URI.
 *
 * Either a code or an error -- RFC 6749 4.1.2 and 4.1.2.1 -- and in both cases
 * the state that was sent, which the caller must check.
 */
struct callback {
    std::string code;
    std::string state;
    std::string error;
    std::string error_description;

    bool ok() const { return error.empty() && !code.empty(); }
};

/**
 * Receive one OAuth2 redirect on loopback.
 *
 * Not a server, and the name is load-bearing.  It binds one loopback port,
 * accepts one connection, reads one request line, writes one fixed page and
 * stops.  Calling it a server is what turns eighty lines into eight hundred:
 * there is no routing, no second request, no keep-alive, and no configuration.
 *
 * 127.0.0.1 explicitly, never INADDR_ANY -- what arrives here is an
 * authorization code, and a receiver reachable from the network receives it
 * from the network.  RFC 8252 7.3 also says to use the address literal rather
 * than "localhost", because "localhost" is whatever the resolver says it is.
 *
 * Plaintext, which is correct: 8.3 says a loopback redirect does not need TLS,
 * and a self-signed certificate would only teach the user to click through a
 * browser warning.
 */
class redirect_receiver {
public:
    /** Bind a loopback port.  0 lets the kernel choose; see redirect_uri(). */
    explicit redirect_receiver(unsigned short port = 0);

    ~redirect_receiver();

    redirect_receiver(const redirect_receiver&) = delete;
    redirect_receiver& operator=(const redirect_receiver&) = delete;

    unsigned short port() const;

    /** "http://127.0.0.1:<port>/" -- register this with the provider. */
    std::string redirect_uri() const;

    /**
     * Wait for the browser, and answer it.
     *
     * @param expect_state  the state that was sent.  A callback carrying
     *                      anything else is refused without being parsed
     *                      further: RFC 6749 10.12 makes this the client's
     *                      only defence against having someone else's
     *                      authorization code planted on it.
     * @param timeout       seconds to wait.  The user may be typing a password
     *                      or approving on a phone, so this is generous by
     *                      default.
     *
     * @throw error on a timeout, a request that is not a GET of the redirect
     *        URI, or a state mismatch.
     *
     * Never logs or prints the code.
     */
    callback wait(const std::string& expect_state, double timeout = 300);

private:
    class impl;
    std::unique_ptr<impl> m_impl;
};

/**
 * The URL to send a browser to.  RFC 6749 4.1.1, with RFC 7636 4.3's fields.
 *
 * @param redirect_uri  where the provider sends the browser back; must be the
 *                      one registered with them, which for a native
 *                      application is a loopback URI
 */
std::string authorize_url(const client& c,
                          const std::string& redirect_uri,
                          const std::string& state,
                          const pkce& p);

/**
 * Turn an authorization code into tokens.  RFC 6749 4.1.3.
 *
 * The redirect URI goes with it and must be the same one: it is not where
 * anything is sent, it is a value the provider compares.
 *
 * @throw denied when the endpoint refuses in the documented way
 */
token exchange(const client& c,
               const std::string& code,
               const pkce& p,
               const std::string& redirect_uri,
               const http::options& o = http::options());

/**
 * The whole flow: open a browser, wait, exchange.
 *
 * Blocking, on purpose.  The application runs it on a thread; sys::ASServent is
 * the house pattern and ASMailBox and ASImapBox are the precedent.  It cannot
 * be anything else without inventing an event loop jlib does not have.
 *
 * @param open  called with the authorization URL.  Opening a browser is the
 *              application's business -- it may be a desktop, a terminal
 *              printing the URL, or a test -- and jlib guessing at xdg-open
 *              would be wrong more often than right.
 *
 * The refresh token is in the result, and storing it is the caller's job.
 */
token authorize(const client& c,
                const std::function<void(const std::string& url)>& open,
                unsigned short port = 0,
                double timeout = 300);

/**
 * The SASL XOAUTH2 message, before base64.
 *
 * Google's and Microsoft's spelling, which is not an RFC:
 *
 *     user=<email>^Aauth=Bearer <token>^A^A
 *
 * where ^A is one octet, 0x01.
 *
 * Written "\001" and not "\x01".  A hexadecimal escape in C++ has no length
 * limit -- it eats every hex digit that follows -- so "\x01auth=" is the single
 * character 0x1A followed by "uth=", and compiles without a word.  The octal
 * form takes at most three digits and stops.
 */
std::string xoauth2(const std::string& user, const std::string& access_token);

}
}
}

#endif // JLIB_NET_OAUTH_HH
