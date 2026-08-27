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

#include <jlib/util/URL.hh>
#include <jlib/util/json.hh>

#include <map>

namespace jlib {
namespace net {
namespace oauth {

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
