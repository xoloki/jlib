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

#include <jlib/net/oauth.hh>

#include <jlib/sys/listener.hh>
#include <jlib/sys/socketstream.hh>

#include <jlib/util/URL.hh>
#include <jlib/util/http.hh>
#include <jlib/util/json.hh>
#include <jlib/util/util.hh>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <map>
#include <memory>
#include <sstream>

namespace jlib {
namespace net {
namespace oauth {

namespace abnf = util::abnf;

namespace {

    /** A minute, because the clock here and the clock there are not the same. */
    const std::time_t SKEW = 60;

    std::time_t clock_now(std::time_t now) {
        return now ? now : std::time(0);
    }

}

bool token::expired(std::time_t now) const {
    if(m_access.empty()) return true;

    // No expires_in in the reply is not "never": it is "the endpoint did not
    // say", and the honest reading is that the token is good until something
    // rejects it.  session::rejected() is how that gets noticed.
    if(m_expires_at == 0) return false;

    return clock_now(now) + SKEW >= m_expires_at;
}

token parse_token_response(const std::string& body,
                           const std::string& sent_refresh,
                           std::time_t now)
{
    util::json::object::ptr o;

    try {
        o = util::json::object::create(body);
    }
    catch(std::exception& e) {
        // Deliberately without the body.  It is a bearer token when the
        // request worked and often still one when it did not, and an exception
        // message goes to a log.
        throw error(std::string("the token endpoint did not answer with JSON: ") +
                    e.what());
    }

    // RFC 6749 5.2.  An error body may arrive with a 400, and some providers
    // send one with a 200, so the body decides rather than the status.
    if(o->has("error")) {
        throw denied(o->get("error").str_or("unknown"),
                     o->get("error_description").str_or(""));
    }

    if(!o->has("access_token"))
        throw error("the token endpoint's reply has no access_token in it");

    token t;

    t.m_access = std::string(o->get("access_token"));
    t.m_type = o->get("token_type").str_or("Bearer");
    t.m_scope = o->get("scope").str_or("");

    // Rotation.  RFC 6749 6: the reply MAY carry a new refresh token, and when
    // it does the old one stops working -- Microsoft rotates on every refresh.
    // Absent, the one that was sent stays good.
    if(o->has("refresh_token")) {
        const std::string fresh = std::string(o->get("refresh_token"));

        t.m_refresh = fresh;
        t.m_rotated = fresh != sent_refresh;
    }
    else {
        t.m_refresh = sent_refresh;
        t.m_rotated = false;
    }

    if(o->has("expires_in")) {
        // int_or, not int: Microsoft's v1 endpoint returns this as the string
        // "3599" and Google as the number 3599.  util::json coerces both now;
        // the default is for the third provider that sends something else,
        // where "no expiry stated" beats a token that expired in 1970.
        const int64_t in = o->get("expires_in").int_or(0);

        if(in > 0) t.m_expires_at = clock_now(now) + static_cast<std::time_t>(in);
    }

    return t;
}

token refresh(const client& c, const std::string& refresh_token,
              const http::options& o)
{
    if(c.token_endpoint.empty()) throw error("no token endpoint");
    if(refresh_token.empty()) throw error("no refresh token to exchange");

    const util::URL endpoint(c.token_endpoint);

    if(endpoint.get_protocol() != "https" && !c.allow_http) {
        // The request about to go out carries the refresh token and, if there
        // is one, the client secret; the access token comes back the same way.
        throw error("refusing to send a credential to a plaintext endpoint: " +
                    c.token_endpoint);
    }

    std::map<std::string, std::string> form;

    form["grant_type"] = "refresh_token";
    form["refresh_token"] = refresh_token;

    if(!c.id.empty()) form["client_id"] = c.id;
    if(!c.secret.empty()) form["client_secret"] = c.secret;
    if(!c.scope.empty()) form["scope"] = c.scope;

    http::fields send;

    send.add("Accept", "application/json");

    // Redirects stay off, whatever the caller's options say.  Following one
    // means POSTing this form -- a refresh token and a client secret -- to a
    // host named by the response, and a Location: is attacker-controlled the
    // moment anything upstream is.
    http::options opts = o;

    opts.redirects = 0;

    const http::Response r = http::post_form(endpoint, form, send, opts);

    // The body is read whatever the status was.  A 400 with a JSON body is the
    // *normal* failure here and it is the only thing that says whether the
    // refresh token is dead or the server merely hiccuped -- which is the whole
    // reason the HTTP client hands a non-2xx back rather than throwing.
    try {
        return parse_token_response(r.body(), refresh_token, 0);
    }
    catch(denied&) {
        throw;
    }
    catch(error&) {
        if(!r.ok()) {
            throw error("the token endpoint answered " +
                        std::to_string(r.status()) + " " + r.reason() +
                        " with nothing a program can act on");
        }

        throw;
    }
}

session::session(client c, std::string refresh, store_fn store)
    : m_client(std::move(c)),
      m_refresh(std::move(refresh)),
      m_store(std::move(store))
{}

const std::string& session::access() {
    if(!m_token.expired()) return m_token.access();

    const token fresh = oauth::refresh(m_client, m_refresh);

    // The refresh token first, and the callback before anything uses the
    // access token beside it.  If the endpoint rotated, the one this object
    // was constructed with is already dead; a crash between here and the next
    // save is a user who has to authorize again.
    m_refresh = fresh.refresh();

    if(m_store && fresh.rotated()) m_store(fresh);

    m_token = fresh;

    return m_token.access();
}

// ------------------------------------------------------------ PKCE and state

std::string random_token(std::size_t bytes) {
    std::string raw(bytes, '\0');

    // RAND_bytes, not std::random_device or rand().  This is the value that
    // makes a stolen authorization code useless, so a predictable one is the
    // same as none -- and OpenSSL is already a hard dependency here, where
    // libsodium is optional and gated.
    if(RAND_bytes(reinterpret_cast<unsigned char*>(&raw[0]),
                  static_cast<int>(bytes)) != 1) {
        throw error("the system random number generator failed");
    }

    return util::base64url::encode(raw);
}

pkce::pkce(std::string verifier)
    : m_verifier(std::move(verifier))
{
    // RFC 7636 4.1: 43 to 128 characters of unreserved.  Checked rather than
    // assumed, because a caller supplying its own is the case where it can be
    // wrong, and a provider's complaint about it says "invalid_request".
    if(m_verifier.length() < 43 || m_verifier.length() > 128)
        throw error("a PKCE verifier is 43 to 128 characters; this one is " +
                    std::to_string(m_verifier.length()));

    for(char c : m_verifier) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') ||
                                c == '-' || c == '.' || c == '_' || c == '~';

        if(!unreserved)
            throw error("a PKCE verifier is unreserved characters only");
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;

    if(EVP_Digest(m_verifier.data(), m_verifier.length(), hash, &len,
                  EVP_sha256(), 0) != 1) {
        throw error("SHA-256 failed");
    }

    // RFC 7636 4.2: base64url of the hash of the *ASCII* verifier, unpadded.
    // The padding matters -- a provider compares the whole string.
    m_challenge = util::base64url::encode(
        std::string(reinterpret_cast<const char*>(hash), len));
}

pkce pkce::generate() {
    // 32 random octets is 43 base64url characters, which is the minimum length
    // RFC 7636 4.1 allows and 256 bits of entropy.
    return pkce(random_token(32));
}

// -------------------------------------------------------- redirect_receiver

class redirect_receiver::impl {
public:
    explicit impl(unsigned short port)
        : listener(port, "127.0.0.1", 1)
    {}

    sys::listener listener;
};

redirect_receiver::redirect_receiver(unsigned short port)
    : m_impl(new impl(port))
{}

redirect_receiver::~redirect_receiver() = default;

unsigned short redirect_receiver::port() const {
    return m_impl->listener.port();
}

std::string redirect_receiver::redirect_uri() const {
    // The address literal, not "localhost".  RFC 8252 7.3: "localhost" is
    // whatever the resolver says it is, and the resolver is not always ours.
    return "http://127.0.0.1:" + std::to_string(port()) + "/";
}

callback redirect_receiver::wait(const std::string& expect_state, double timeout) {
    std::unique_ptr<sys::socketstream> browser;

    try {
        browser = m_impl->listener.accept_stream(timeout);
    }
    catch(std::exception& e) {
        throw error(std::string("waiting for the browser: ") + e.what());
    }

    if(!browser)
        throw error("no redirect arrived within " + std::to_string(timeout) +
                    " seconds");

    // Reading is bounded twice over: a deadline, and a cap on the request line.
    // Whatever connected is not necessarily a browser.
    browser->set_timeout(10);

    std::string line;

    std::getline(*browser, line);

    while(!line.empty() && line.back() == '\r') line.pop_back();

    if(line.length() > 8192) throw error("the redirect's request line is absurd");

    // "GET /?code=...&state=... HTTP/1.1".  Read with the grammar rather than
    // with find(): request-line is RFC 9112 3, and the pieces come out of the
    // match instead of out of offsets counted by hand.
    const abnf::parse_result p =
        util::http::grammar().at("request-line").try_parse(line);

    if(!p) throw error("what connected to the redirect port did not send a "
                       "request line");

    const abnf::match m = p.root();

    if(m["method"].str() != "GET")
        throw error("the redirect was not a GET");

    // The target is origin-form: an absolute path and a query.  URL learned to
    // parse a relative reference two branches ago, which is exactly this.
    util::URL target;

    try {
        target.parse_reference(m["request-target"].str());
    }
    catch(std::exception&) {
        throw error("the redirect's request target is not a URI reference");
    }

    callback back;

    back.code = target["code"];
    back.state = target["state"];
    back.error = target["error"];
    back.error_description = target["error_description"];

    // The state first, and before anything else is believed.  RFC 6749 10.12:
    // this is the client's only defence against someone else's authorization
    // code being planted on it, and the check is worthless if it happens after
    // the code has been used.
    const bool state_ok = !expect_state.empty() && back.state == expect_state;

    // One fixed page, whatever happened, because the person is looking at a
    // browser and an empty response is indistinguishable from a crash.  It
    // never contains the code: a URL bar is shoulder-surfable and a page is
    // saveable.
    const std::string body =
        state_ok && back.ok()
        ? "<html><body><h1>Signed in</h1><p>You can close this window and "
          "return to the application.</p></body></html>"
        : "<html><body><h1>Sign-in failed</h1><p>Return to the application; "
          "it has the details.</p></body></html>";

    std::ostringstream reply;

    reply << "HTTP/1.1 200 OK\r\n"
          << "Content-Type: text/html; charset=utf-8\r\n"
          << "Content-Length: " << body.length() << "\r\n"
          << "Connection: close\r\n"
          << "\r\n"
          << body;

    *browser << reply.str() << std::flush;
    browser->close();

    if(!state_ok) {
        throw error("the redirect's state does not match the one that was sent; "
                    "refusing it");
    }

    if(!back.error.empty()) {
        throw denied(back.error, back.error_description);
    }

    if(back.code.empty()) throw error("the redirect carried neither a code nor an error");

    return back;
}

// ----------------------------------------------------- the code grant itself

std::string authorize_url(const client& c,
                          const std::string& redirect_uri,
                          const std::string& state,
                          const pkce& p)
{
    if(c.authorize_endpoint.empty()) throw error("no authorization endpoint");
    if(c.id.empty()) throw error("no client id");
    if(state.empty()) throw error("no state; see RFC 6749 10.12");

    const util::URL endpoint(c.authorize_endpoint);

    if(endpoint.get_protocol() != "https" && !c.allow_http) {
        // Not because a secret goes out on it -- nothing here does -- but
        // because whoever can rewrite this URL chooses where the user types
        // their password.
        throw error("refusing to send a user to a plaintext authorization "
                    "endpoint: " + c.authorize_endpoint);
    }

    std::map<std::string, std::string> query;

    query["response_type"] = "code";
    query["client_id"] = c.id;
    query["redirect_uri"] = redirect_uri;
    query["state"] = state;
    query["code_challenge"] = p.challenge();
    query["code_challenge_method"] = pkce::method();

    if(!c.scope.empty()) query["scope"] = c.scope;

    // form_encode, which is the encoding a query string uses: space is "+".
    // The separator is whatever the endpoint already has -- a provider is
    // entitled to put its own parameters in the URL it publishes.
    const std::string sep = endpoint.get_qs().empty() ? "?" : "&";

    return c.authorize_endpoint + sep + http::form_encode(query);
}

token exchange(const client& c,
               const std::string& code,
               const pkce& p,
               const std::string& redirect_uri,
               const http::options& o)
{
    if(c.token_endpoint.empty()) throw error("no token endpoint");
    if(code.empty()) throw error("no authorization code to exchange");

    const util::URL endpoint(c.token_endpoint);

    if(endpoint.get_protocol() != "https" && !c.allow_http) {
        throw error("refusing to send a credential to a plaintext endpoint: " +
                    c.token_endpoint);
    }

    std::map<std::string, std::string> form;

    form["grant_type"] = "authorization_code";
    form["code"] = code;
    form["redirect_uri"] = redirect_uri;
    form["code_verifier"] = p.verifier();

    if(!c.id.empty()) form["client_id"] = c.id;
    if(!c.secret.empty()) form["client_secret"] = c.secret;

    http::fields send;

    send.add("Accept", "application/json");

    http::options opts = o;

    opts.redirects = 0;

    const http::Response r = http::post_form(endpoint, form, send, opts);

    // An authorization code is single-use, so there is no refresh token to
    // fall back on here: whatever the reply says about one is all there is.
    try {
        return parse_token_response(r.body(), std::string(), 0);
    }
    catch(denied&) {
        throw;
    }
    catch(error&) {
        if(!r.ok()) {
            throw error("the token endpoint answered " +
                        std::to_string(r.status()) + " " + r.reason() +
                        " with nothing a program can act on");
        }

        throw;
    }
}

token authorize(const client& c,
                const std::function<void(const std::string&)>& open,
                unsigned short port,
                double timeout)
{
    if(!open) throw error("no way to open a browser was given");

    // Bound before the URL is built, because the URL has to name the port.
    redirect_receiver receiver(port);

    const pkce p = pkce::generate();
    const std::string state = random_token(32);
    const std::string uri = receiver.redirect_uri();

    open(authorize_url(c, uri, state, p));

    const callback back = receiver.wait(state, timeout);

    return exchange(c, back.code, p, uri);
}

std::string xoauth2(const std::string& user, const std::string& access_token) {
    // "\001", never "\x01".  A hexadecimal escape in C++ has no length limit --
    // it consumes every hex digit that follows -- so "\x01auth=" is the one
    // character 0x1A followed by "uth=", and it compiles without a warning.
    // The octal form takes three digits at most and stops.
    return "user=" + user + "\001auth=Bearer " + access_token + "\001\001";
}

}
}
}
