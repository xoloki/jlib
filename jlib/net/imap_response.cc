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

#include <jlib/net/imap_response.hh>
#include <jlib/net/rfc3501.hh>

#include <jlib/util/abnf.hh>
#include <jlib/util/util.hh>

#include <istream>

namespace jlib {
namespace net {
namespace imap {

namespace {

using util::abnf::grammar;
using util::abnf::match;
using util::abnf::options;
using util::abnf::rule;

/**
 * The grammar, with literal supplied in combinators.
 *
 * RFC 3501 4.3 writes the length constraint in a comment because ABNF cannot
 * express it, so rfc3501.hh references `literal` and never defines it and
 * counted() defines it here.  check() runs after, not before -- an undefined
 * rule is exactly what it would object to.
 *
 * Function-local for the reason at crypt/curve.hh:42.
 */
const grammar& responses()
{
    static grammar g = [] {
        grammar g = util::abnf::compile(rfc3501::RESPONSE);

        const rule size = util::abnf::as("literal-size",
                                         +util::abnf::core::DIGIT());

        g.define("literal",
                 util::abnf::lit("{") >> size >> util::abnf::lit("}")
                 >> util::abnf::core::CRLF()
                 >> util::abnf::as("literal-text", util::abnf::counted(size)));

        g.check();

        return g;
    }();

    return g;
}

options parse_options()
{
    options o;

    o.captures = options::capture_policy::listed;
    o.capture_only = {
        "continue-req", "response-tagged", "response-data",
        "rtag", "cond-name", "resp-text-code", "text",
        "mailbox-data", "capability-data", "list-name", "count-name",
        "mailbox", "delim", "capability", "message-data", "number",
        "flag-list", "flag",
        "msg-att", "msg-att-item", "att-name", "att-section", "att-value",
        "att-group",
        "nstring", "nil", "quoted-body", "literal-text",
    };

    return o;
}

/** The text of an astring or an attribute value, however it was written. */
std::string text_of(const match& m)
{
    // A parenthesised value is handed back as written: jlib does not
    // interpret an ENVELOPE or a BODYSTRUCTURE, and reaching inside for the
    // first string it finds -- which is what this did before the test caught
    // it -- turns "(\"d\" \"s\" NIL NIL)" into "d".
    if(m["att-group"]) return m.str();

    if(const match q = m["quoted-body"]) {
        // RFC 3501 4.3: inside a quoted string, only DQUOTE and backslash may
        // be escaped, and each stands for itself.
        std::string out;
        const std::string_view s = q.text();

        for(std::size_t i = 0; i < s.size(); i++) {
            if(s[i] == '\\' && i + 1 < s.size()) i++;

            out += s[i];
        }

        return out;
    }

    if(const match l = m["literal-text"]) return l.str();
    if(m["nil"])                          return std::string();

    return m.str();
}

}

// ------------------------------------------------------------------ reading

bool literal_size(const std::string& line, std::size_t& n)
{
    // The introducer is the last thing on the line, so this is where it has to
    // start if it is anywhere.
    const std::string::size_type b = line.rfind('{');

    if(b == line.npos) return false;

    static const grammar& g = [] () -> const grammar& {
        static grammar h = [] {
            grammar h = util::abnf::compile(
                "literal-introducer = \"{\" number [\"+\"] \"}\"\r\n"
                "number             = 1*DIGIT\r\n");
            h.check();

            return h;
        }();

        return h;
    }();

    options o;
    o.captures = options::capture_policy::listed;
    o.capture_only = { "number" };

    const util::abnf::parse_result r =
        g.at("literal-introducer").try_parse(line.substr(b), o);

    // A whole-input match, so "{12} more words" is not a literal and neither
    // is "{}", "{-1}" or "{12x}".
    if(!r) return false;

    try {
        const unsigned long long v = std::stoull(r.root()["number"].str());

        // This decides how many octets to read and it comes from the network.
        if(v > 0x7FFFFFFFull) return false;

        n = static_cast<std::size_t>(v);
    }
    catch(std::exception&) {
        return false;
    }

    return true;
}

std::string read(std::istream& is)
{
    std::string out;

    for(;;) {
        std::string line;

        if(!std::getline(is, line)) {
            throw error("connection ended in the middle of a response");
        }

        // Exactly one CRLF comes off and goes straight back on.  A literal's
        // octets are message content and a line of them may genuinely end in
        // CR; sys::getline erases every trailing one.
        const bool crlf = !line.empty() && line.back() == '\r';

        if(crlf) line.pop_back();

        out += line;
        out += crlf ? "\r\n" : "\n";

        std::size_t n = 0;

        if(!literal_size(line, n)) return out;

        // Exactly n octets, whatever they are.
        std::string body(n, '\0');

        is.read(&body[0], static_cast<std::streamsize>(n));

        if(is.gcount() != static_cast<std::streamsize>(n)) {
            throw error("connection ended inside a literal");
        }

        out += body;
    }
}

// ------------------------------------------------------------------ parsing

struct reader {

    static void condition_of(response& r, const match& m)
    {
        const match c = m["cond-name"];

        if(!c) return;

        const std::string name = util::upper(c.str());

        if(name == "OK")           r.m_condition = response::condition::ok;
        else if(name == "NO")      r.m_condition = response::condition::no;
        else if(name == "BAD")     r.m_condition = response::condition::bad;
        else if(name == "PREAUTH") r.m_condition = response::condition::preauth;
        else if(name == "BYE")     r.m_condition = response::condition::bye;

        if(const match code = m["resp-text-code"]) r.m_code = code.str();
        if(const match text = m["text"])           r.m_text = text.str();
    }

    static void flags_of(response& r, const match& m)
    {
        for(const match& f : m.all("flag")) r.m_flags.push_back(f.str());
    }

    static void data_of(response& r, const match& m)
    {
        // capability-data is a sibling of mailbox-data in response-data, not
        // a part of it -- so looking for "CAPABILITY" inside mailbox-data,
        // which is what this did, found nothing and silently produced a
        // response with no name and no capabilities.
        if(const match c = m["capability-data"]) {
            r.m_name = "CAPABILITY";

            for(const match& one : c.all("capability")) {
                r.m_capabilities.push_back(one.str());
            }

            return;
        }

        const match d = m["mailbox-data"];

        if(d) {
            if(const match n = d["list-name"])       r.m_name = util::upper(n.str());
            else if(const match c = d["count-name"]) r.m_name = util::upper(c.str());
            else if(util::ibegins(d.str(), "FLAGS")) r.m_name = "FLAGS";
            else if(util::ibegins(d.str(), "SEARCH")) r.m_name = "SEARCH";
            else if(util::ibegins(d.str(), "STATUS")) r.m_name = "STATUS";

            flags_of(r, d);

            if(const match mb = d["mailbox"]) r.m_mailbox = text_of(mb);

            if(const match dl = d["delim"]) {
                // DQUOTE QUOTED-CHAR DQUOTE, so the delimiter is the middle.
                const std::string s = dl.str();

                r.m_delimiter = s.size() >= 3 ? s.substr(1, s.size() - 2)
                                              : std::string();
            }

            for(const match& n : d.all("number")) {
                if(r.m_name == "SEARCH") r.m_numbers.push_back(std::stoul(n.str()));
                else                     r.m_number = std::stoul(n.str());
            }

            return;
        }

        const match msg = m["message-data"];

        if(!msg) return;

        if(const match n = msg["number"]) r.m_number = std::stoul(n.str());

        r.m_name = msg["msg-att"] ? "FETCH" : "EXPUNGE";

        for(const match& item : msg.all("msg-att-item")) {
            if(const match fl = item["flag-list"]) {
                flags_of(r, fl);
                r.m_attributes["FLAGS"] = fl.str();

                continue;
            }

            const match name = item["att-name"];

            if(!name) continue;

            std::string key = util::upper(name.str());

            // The section is part of what was asked for: a caller that fetched
            // BODY[HEADER] looks for BODY[HEADER].
            if(const match sec = item["att-section"]) key += sec.str();

            const match value = item["att-value"];

            r.m_attributes[key] = value ? text_of(value) : std::string();
        }
    }

    static response of(const match& m, std::string_view raw)
    {
        response r;

        r.m_raw = std::string(raw);

        if(m["continue-req"]) {
            r.m_kind = response::kind::continuation;

            if(const match t = m["text"]) r.m_text = t.str();
            if(const match c = m["resp-text-code"]) r.m_code = c.str();

            return r;
        }

        if(const match t = m["response-tagged"]) {
            r.m_kind = response::kind::tagged;
            r.m_tag = m["rtag"].str();

            condition_of(r, t);

            return r;
        }

        r.m_kind = response::kind::untagged;

        const match d = m["response-data"];

        condition_of(r, d);
        data_of(r, d);

        return r;
    }
};

response response::parse(std::string_view s)
{
    try {
        return reader::of(responses().at("response").parse(s, parse_options()), s);
    }
    catch(util::abnf::budget_exceeded& e) {
        throw error(std::string("gave up reading a response: ") + e.what());
    }
    catch(util::abnf::error& e) {
        // Not e.what(): the expected set for this grammar is most of ASCII.
        throw error("not an IMAP response at column " + std::to_string(e.column())
                    + ": " + e.context_line());
    }
}

response response::read(std::istream& is)
{
    return parse(imap::read(is));
}

bool response::valid(std::string_view s)
{
    options o;

    o.captures = options::capture_policy::none;

    return static_cast<bool>(responses().at("response").try_parse(s, o));
}

bool response::ok() const
{
    return m_condition == condition::ok || m_condition == condition::preauth;
}

}
}
}
