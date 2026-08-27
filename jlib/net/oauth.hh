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

#include <ctime>
#include <functional>
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
 * Not here: the authorization-code flow that *obtains* the first refresh token.
 * That needs a browser, a loopback listener and PKCE, and it is a branch of its
 * own.  Until then a refresh token has to come from somewhere else -- a
 * provider's playground, another client, a script -- and this reads mail with
 * it, which is the smallest useful thing.
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
