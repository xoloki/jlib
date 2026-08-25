/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */

#include <jlib/net/content_type.hh>
#include <jlib/net/rfc2045.hh>
#include <jlib/net/rfc5322.hh>

#include <jlib/util/abnf.hh>

#include <algorithm>
#include <map>
#include <set>

namespace jlib {
namespace net {

namespace {

using util::abnf::grammar;
using util::abnf::match;
using util::abnf::options;

/**
 * The grammar, built on first use.
 *
 * Function-local for the reason at crypt/curve.hh:42, and read-only once
 * check() has run -- see the same note in address.cc.
 */
const grammar& mime()
{
    static grammar g = [] {
        grammar g = util::abnf::compile(std::string(rfc5322::LEXICAL) +
                                        rfc2045::CONTENT);
        g.check();

        return g;
    }();

    return g;
}

options parse_options()
{
    options o;

    o.captures = options::capture_policy::listed;
    o.capture_only = { "mt-type", "mt-subtype", "disp-type",
                       "parameter", "attribute", "section", "extended",
                       "value", "token", "qs-body" };

    return o;
}

std::string lower(std::string_view s)
{
    std::string out(s);

    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    return out;
}

/** RFC 5322 3.2.1 and 3.2.2: undo quoted-pair, and unfold. */
std::string unescape(std::string_view s)
{
    std::string out;

    out.reserve(s.size());

    for(std::size_t i = 0; i < s.size(); ++i) {
        if(s[i] == '\\' && i + 1 < s.size())      out += s[++i];
        else if(s[i] != '\r' && s[i] != '\n')     out += s[i];
    }

    return out;
}

/** The text of a value, whether it was written as a token or quoted. */
std::string value_of(const match& v)
{
    const match q = v.child("qs-body");

    if(q) return unescape(q.text());

    const match t = v.child("token");

    return t ? t.str() : std::string();
}

int hex(char c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;

    return -1;
}

/**
 * RFC 2231 4's ext-octet: "%" 2HEXDIG.
 *
 * A stray "%" that is not followed by two hex digits is kept as a "%" rather
 * than dropped.  The alternative is to lose a character out of a filename
 * because a sender wrote "100%.txt" and forgot to escape it.
 */
std::string percent_decode(std::string_view s)
{
    std::string out;

    out.reserve(s.size());

    for(std::size_t i = 0; i < s.size(); ++i) {
        if(s[i] == '%' && i + 2 < s.size() &&
           hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        }
        else {
            out += s[i];
        }
    }

    return out;
}

/** One parameter as the grammar matched it, before sections are joined. */
struct raw {
    std::string name;
    long section = -1;      ///< -1 when the name carried no "*n"
    bool extended = false;
    std::string text;
};

/**
 * RFC 2231 4: charset'language'value, on the first section only.
 *
 * The two apostrophes are mandatory in the initial segment and absent from
 * every continuation, which is what makes a continuation distinguishable from
 * a segment that happens to contain one.
 */
void split_extended(std::string_view s, std::string& charset, std::string& rest)
{
    const std::size_t a = s.find('\'');

    if(a == s.npos) { rest = std::string(s); return; }

    const std::size_t b = s.find('\'', a + 1);

    if(b == s.npos) { rest = std::string(s); return; }

    charset = lower(s.substr(0, a));
    rest = std::string(s.substr(b + 1));   // the language tag is discarded
}

/** Join the sections of each name, decoding what is marked extended. */
content_type::parameter_list assemble(const std::vector<raw>& in)
{
    content_type::parameter_list out;
    std::map<std::string, std::size_t> at;

    // Sections are joined in numeric order however they arrived, but the
    // parameters themselves keep the order the header wrote them in.
    std::map<std::string, std::map<long, const raw*>> parts;

    for(const raw& r : in) {
        if(!at.count(r.name)) {
            at[r.name] = out.size();
            out.push_back({ r.name, std::string(), std::string() });
        }

        // First wins.  RFC 2045 does not say what a repeated parameter means,
        // and taking the last would let a second "charset" or "boundary"
        // appended to a header override the first -- which is a parameter
        // smuggling primitive, not a parse.
        parts[r.name].emplace(r.section, &r);
    }

    for(const auto& p : parts) {
        content_type::parameter& q = out[at[p.first]];
        bool first = true;

        for(const auto& s : p.second) {
            const raw& r = *s.second;

            if(!r.extended) {
                q.value += r.text;
            }
            else if(first) {
                std::string rest;

                split_extended(r.text, q.charset, rest);
                q.value += percent_decode(rest);
            }
            else {
                q.value += percent_decode(r.text);
            }

            first = false;
        }
    }

    return out;
}

}

// -------------------------------------------------------------- the reader

struct mime_reader {

    static content_type read(const match& m, bool disposition)
    {
        content_type c;

        if(disposition) {
            c.m_type = lower(m.child("disp-type").text());
        }
        else {
            c.m_type = lower(m.child("mt-type").text());
            c.m_subtype = lower(m.child("mt-subtype").text());
        }

        std::vector<raw> params;

        for(const match& p : m.all("parameter")) {
            raw r;

            r.name = lower(p.child("attribute").text());

            const match s = p.child("section");

            // "*0" -- the "*" is part of the match, so skip it.
            if(s) r.section = std::stol(s.str().substr(1));

            r.extended = static_cast<bool>(p.child("extended"));
            r.text = value_of(p.child("value"));

            params.push_back(std::move(r));
        }

        c.m_parameters = assemble(params);

        return c;
    }
};

// ---------------------------------------------------------------- exception

content_type::exception::exception(const std::string& msg, std::string text,
                                   std::size_t offset)
    : std::runtime_error("jlib::net::content_type::exception: " + msg),
      m_text(std::move(text)),
      m_offset(offset)
{
}

// -------------------------------------------------------------------- parse

namespace {

match run(const std::string& rule, std::string_view s)
{
    try {
        return mime().at(rule).parse(s, parse_options());
    }
    catch(util::abnf::budget_exceeded& e) {
        throw content_type::exception(std::string("gave up reading a header: ")
                                      + e.what(), std::string(s), e.offset());
    }
    catch(util::abnf::error& e) {
        // The expected set for this grammar is every tchar, which tells a
        // caller nothing.  Where it stopped does.
        throw content_type::exception("not a MIME header at column "
                                      + std::to_string(e.column()) + "\n  "
                                      + e.context_line() + "\n  "
                                      + std::string(e.column() - 1, ' ') + "^",
                                      std::string(s), e.offset());
    }
}

}

content_type content_type::parse(std::string_view s)
{
    return mime_reader::read(run("media-type", s), false);
}

content_type content_type::parse_disposition(std::string_view s)
{
    return mime_reader::read(run("disposition", s), true);
}

bool content_type::valid(std::string_view s)
{
    try {
        parse(s);
        return true;
    }
    catch(exception&) {
        return false;
    }
}

// ------------------------------------------------------------------ reading

std::string content_type::essence() const
{
    return m_subtype.empty() ? m_type : m_type + "/" + m_subtype;
}

bool content_type::is(std::string_view type) const
{
    return m_type == lower(type);
}

bool content_type::is(std::string_view type, std::string_view subtype) const
{
    return m_type == lower(type) && m_subtype == lower(subtype);
}

bool content_type::has(std::string_view name) const
{
    const std::string n = lower(name);

    for(const parameter& p : m_parameters) {
        if(p.name == n) return true;
    }

    return false;
}

std::string content_type::get(std::string_view name,
                              const std::string& fallback) const
{
    const std::string n = lower(name);

    for(const parameter& p : m_parameters) {
        if(p.name == n) return p.value;
    }

    return fallback;
}

std::string content_type::charset_of(std::string_view name) const
{
    const std::string n = lower(name);

    for(const parameter& p : m_parameters) {
        if(p.name == n) return p.charset;
    }

    return std::string();
}

// ------------------------------------------------------------------ writing

void content_type::set(std::string name, std::string value)
{
    const std::string n = lower(name);

    for(parameter& p : m_parameters) {
        if(p.name == n) {
            p.value = std::move(value);
            p.charset.clear();

            return;
        }
    }

    m_parameters.push_back({ n, std::move(value), std::string() });
}

namespace {

const std::string& tchar()
{
    static const std::string s =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        "!#$%&'*+-.^_`{|}~";

    return s;
}

std::string quote(std::string_view s)
{
    std::string out = "\"";

    for(char c : s) {
        if(c == '"' || c == '\\') out += '\\';
        out += c;
    }

    return out + "\"";
}

}

namespace {

/**
 * RFC 2231 4, for a value a quoted-string cannot hold.
 *
 * A parameter value is ASCII: qtext stops at %d126, so a filename with a
 * non-ASCII octet in it written as "naïve.pdf" produces a header that this
 * very parser refuses.  Anything with a high byte, or anything that arrived
 * with a charset attached, goes back out the way it came in.
 */
std::string extended(const content_type::parameter& p)
{
    std::string out = p.name + "*=" + p.charset + "''";

    for(unsigned char c : p.value) {
        if(c >= 0x80 || tchar().find(static_cast<char>(c)) == std::string::npos) {
            static const char* d = "0123456789ABCDEF";

            out += '%';
            out += d[c >> 4];
            out += d[c & 0xF];
        }
        else {
            out += static_cast<char>(c);
        }
    }

    return out;
}

}

std::string content_type::str() const
{
    std::string out = essence();

    for(const parameter& p : m_parameters) {
        bool plain = !p.value.empty();
        bool ascii = true;

        for(unsigned char c : p.value) {
            if(c >= 0x80) ascii = false;
            if(tchar().find(static_cast<char>(c)) == std::string::npos) plain = false;
        }

        if(!ascii || !p.charset.empty()) {
            out += "; " + extended(p);
        }
        else {
            out += "; " + p.name + "=" + (plain ? p.value : quote(p.value));
        }
    }

    return out;
}

// --------------------------------------------------------------- multipart

std::vector<std::string> split_multipart(std::string_view body,
                                         std::string_view boundary)
{
    std::vector<std::string> out;

    if(boundary.empty()) return out;

    const std::string mark = "--" + std::string(boundary);

    // Where the current part's content begins; npos until the first delimiter
    // has been seen, because everything before that is the preamble.
    std::size_t begin = std::string_view::npos;

    for(std::size_t i = body.find(mark); i != body.npos;
        i = body.find(mark, i + 1)) {

        // RFC 2046 5.1.1: the delimiter is CRLF "--" boundary, so it has to
        // start a line.  Without this, a boundary appearing inside a part's
        // content would split the message.
        std::size_t eol = 0;

        if(i >= 2 && body.compare(i - 2, 2, "\r\n") == 0)      eol = 2;
        else if(i >= 1 && body[i - 1] == '\n')                 eol = 1;
        else if(i != 0)                                        continue;

        std::size_t j = i + mark.size();
        const bool last = body.compare(j, 2, "--") == 0;

        if(last) j += 2;

        // Transport padding, then the line break that ends the delimiter
        // line.  Both belong to the delimiter, not to what follows.
        while(j < body.size() && (body[j] == ' ' || body[j] == '\t')) j++;

        if(body.compare(j, 2, "\r\n") == 0)          j += 2;
        else if(j < body.size() && body[j] == '\n')  j += 1;
        else if(j < body.size() && !last)            continue;

        // The line break before this delimiter ended the previous part and is
        // not part of it.
        if(begin != std::string_view::npos) {
            out.push_back(std::string(body.substr(begin, i - eol - begin)));
        }

        if(last) return out;

        begin = j;
    }

    // No closing delimiter.  What has been collected is real; the unterminated
    // last part is not, and a truncated message should not produce a part
    // whose end nothing marked.
    return out;
}

}
}
