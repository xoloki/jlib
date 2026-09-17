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

#ifndef JLIB_APPS_JHTTPD_HH
#define JLIB_APPS_JHTTPD_HH

#include <jlib/net/http_server.hh>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <mutex>
#include <string>

/**
 * jhttpd's logging: the Combined Log Format, and the escaping it needs.
 *
 * In a header so a test can drive it without the program. The formatting is
 * the part with decisions in it; the program around it is argument parsing.
 */
namespace jhttpd {

/**
 * One field, safe to put between quotes on a line of a log file.
 *
 * **This is the whole security content of logging**, and #270 records it as
 * the first thing to look at in a jhttpd audit, so it is done here rather than
 * filed.
 *
 * A Combined line is delimited by newlines and its client-supplied fields by
 * double quotes. `method`, `target`, `referer` and `user_agent` all come from
 * the client. A log an attacker can write is worse than no log, because it is
 * believed -- and it is believed by an operator during an incident, which is
 * the worst possible moment to be reading fiction.
 *
 * ## What actually gets through, measured rather than assumed
 *
 * The obvious attack -- a newline, forging whole entries -- **cannot happen**,
 * and it is worth knowing why before trusting that. `field-value` is
 * `*field-content` over `field-vchar`, which is VCHAR or obs-text, so a raw CR
 * or LF or any other control character in a header value is refused by the
 * grammar with a 400 before anything is logged. A request-target arrives
 * percent-encoded, so it cannot carry one either. Escaping does not close that
 * hole; the parser already did.
 *
 * Two things do get through, and both are why this function exists:
 *
 *   - **A double quote.** `"` is an ordinary VCHAR and perfectly legal in a
 *     User-Agent. Unescaped, `x" 999 0 "-" "forged` produces a line whose
 *     fields parse as a different request with a different status. Nothing
 *     upstream refuses it, because there is nothing wrong with it as HTTP.
 *   - **Bytes above 0x7F**, which `obs-text` allows. They are not dangerous,
 *     but they are not necessarily valid UTF-8, and a log file that is not
 *     text is a log file that breaks whatever reads it next.
 *
 * So the live vector is field-structure forgery rather than line forgery, and
 * saying so precisely is the difference between a guard somebody keeps and one
 * somebody deletes as paranoia.
 *
 * ## The escaping is Apache's, deliberately
 *
 * Backslash escapes, `\xHH` for anything else non-printable -- which is what
 * Apache writes and therefore what log parsers already read. A format that is
 * safe and unreadable by existing tools would defeat the reason for choosing
 * Combined in the first place.
 *
 */




/**
 * A log file that several threads may write to.
 *
 * One `write(2)`-sized append under a mutex, because the blocking server
 * answers on a pool and the async one answers on the reactor, and two
 * half-lines interleaved are two lines nobody can parse.
 *
 * **The line is built before the lock is taken.** Formatting is the expensive
 * part and holding a mutex across it would serialise the servers' request
 * handling behind their own log -- which on the async server means serialising
 * the reactor thread, the one thread that must not wait for a disk.
 */
class logfile {
public:
    logfile() {}

    /** Empty path means discard, which is what --access-log "" asks for. */
    bool open(const std::string& path);

    void write(const std::string& line);

    /** Declared above, defined below: the class has to exist first. */

    bool wanted() const { return m_out.is_open(); }

private:
    std::mutex    m_lock;
    std::ofstream m_out;
};



inline std::string escaped(const std::string& field) {
    // A field with nothing to escape is the overwhelmingly common case, and
    // the common case should not allocate a second string to discover that.
    bool clean = true;

    for(std::size_t i = 0; i < field.size() && clean; i++) {
        const unsigned char c = static_cast<unsigned char>(field[i]);

        if(c < 0x20 || c >= 0x7F || c == '"' || c == '\\') clean = false;
    }

    if(clean) return field;

    std::string out;

    out.reserve(field.size() + 8);

    for(std::size_t i = 0; i < field.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(field[i]);

        switch(c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if(c < 0x20 || c >= 0x7F) {
                static const char* const hex = "0123456789abcdef";
                char esc[5] = { '\\', 'x', hex[c >> 4], hex[c & 0xF], 0 };

                out += esc;
            }
            else out += char(c);
        }
    }

    return out;
}

/** Combined's date: 17/Sep/2026:12:00:00 +0000, and always UTC. */
inline std::string log_date(std::time_t when) {
    static const char* const MONTH[] = { "Jan", "Feb", "Mar", "Apr", "May",
                                         "Jun", "Jul", "Aug", "Sep", "Oct",
                                         "Nov", "Dec" };
    std::tm tm;

    // UTC rather than local time.  A log that changes meaning twice a year is
    // one whose timestamps cannot be compared across the change, and the hour
    // that repeats in autumn is the one an incident will land in.
    ::gmtime_r(&when, &tm);

    char buf[64];

    std::snprintf(buf, sizeof buf, "%02d/%s/%04d:%02d:%02d:%02d +0000",
                  tm.tm_mday, MONTH[tm.tm_mon], tm.tm_year + 1900,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);

    return buf;
}

inline std::string combined(const jlib::net::http::server::access& a,
                     std::time_t when)
{
    std::ostringstream o;

    // `-` for anything absent, which is the format's convention and is why a
    // field is never simply empty: an empty quoted field and a missing one
    // read differently to every tool that parses these.
    const std::string user = a.user.empty() ? "-" : escaped(a.user);
    const std::string referer = a.referer.empty() ? "-" : escaped(a.referer);
    const std::string agent = a.user_agent.empty() ? "-" : escaped(a.user_agent);

    // The request line as three escaped pieces rather than one: a client that
    // sends a target with a space in it would otherwise produce a line whose
    // fields do not line up, and the space is not escapable without inventing
    // a format.
    std::string request;

    if(a.method.empty() && a.target.empty()) request = "-";
    else {
        request = escaped(a.method) + " " + escaped(a.target);

        if(!a.version.empty()) request += " " + escaped(a.version);
    }

    o << (a.peer.empty() ? "-" : a.peer)
      << " - "                       // identd, which nothing has run since 1995
      << user
      << " [" << log_date(when) << "] "
      << "\"" << request << "\" "
      << a.status << " "
      << a.bytes
      << " \"" << referer << "\""
      << " \"" << agent << "\"";

    return o.str();
}

inline bool logfile::open(const std::string& path) {
    if(path.empty()) return true;

    m_out.open(path.c_str(), std::ios::out | std::ios::app);

    return m_out.is_open();
}

inline void logfile::write(const std::string& line) {
    if(!m_out.is_open()) return;

    // Built by the caller; this only appends.  See the note on the class about
    // why the formatting is outside the lock.
    std::lock_guard<std::mutex> hold(m_lock);

    m_out << line << "\n";
    m_out.flush();
}

}

#endif
