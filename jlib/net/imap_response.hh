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

#ifndef JLIB_NET_IMAP_RESPONSE_HH
#define JLIB_NET_IMAP_RESPONSE_HH

#include <cstddef>
#include <iosfwd>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace jlib {
namespace net {

/**
 * IMAP4rev1 responses, read against RFC 3501's grammar.
 *
 * The grammar is in jlib/net/rfc3501.hh.  What this adds is the part a
 * grammar cannot do on its own: getting a whole response off the wire in the
 * first place.
 *
 * ## A response is not a line
 *
 *     * 12 FETCH (BODY[HEADER] {13}
 *     From: a@b.c
 *     )
 *
 * That is one response over three lines, and the thirteen octets after the
 * "{13}" are message content that may contain anything at all -- including a
 * line that looks exactly like the tagged completion the client is waiting
 * for.  Reading until a line starts with the tag, which is what Imap4 did for
 * twenty-five years, therefore stops in the wrong place and every command
 * after it reads part of somebody's email as its response.
 *
 * read() follows the literals.  Nothing else can.
 */
namespace imap {

/**
 * The octet count of the literal a response line ends with.
 *
 * RFC 3501 4.3: "{" number "}" CRLF, then that many octets.  Returns false
 * when the line does not end in one -- which is the case that used to give
 * zero and read no octets at all, because it went through util::slice, and
 * slice returns its whole input when the delimiters are not there.
 *
 * A count over 2^31-1 is refused rather than clamped: it decides how many
 * octets to read and it comes from the network.
 */
bool literal_size(const std::string& line, std::size_t& n);

/**
 * Read one complete response, following any literals in it.
 *
 * Everything up to and including the CRLF that ends the response, with each
 * literal's octets in place.  Throws imap::error if the stream ends first --
 * a truncated response is not a short one.
 */
std::string read(std::istream& is);

/** A response that could not be read. */
class error : public std::runtime_error {
public:
    explicit error(const std::string& msg)
        : std::runtime_error("jlib::net::imap::error: " + msg) {}
};

/**
 * One parsed response.
 *
 * Every accessor is empty or zero for a response of a kind that does not
 * carry that piece; nothing throws for asking.
 */
class response {
public:
    /** RFC 3501 7: which of the three shapes this is. */
    enum class kind { tagged, untagged, continuation };

    /** The condition of an OK/NO/BAD/PREAUTH/BYE, or none. */
    enum class condition { none, ok, no, bad, preauth, bye };

    response() = default;

    /** Parse one complete response.  Throws imap::error. */
    static response parse(std::string_view s);

    /** read() then parse(). */
    static response read(std::istream& is);

    static bool valid(std::string_view s);

    kind type() const { return m_kind; }

    /** The tag of a tagged response, or "" for the other two. */
    const std::string& tag() const { return m_tag; }

    condition status() const { return m_condition; }

    /** OK or PREAUTH.  False for a response that carries no condition. */
    bool ok() const;

    /** The "[UIDVALIDITY 3857529045]" of a resp-text, without its brackets. */
    const std::string& code() const { return m_code; }

    /** The human-readable remainder.  Not for a program to act on. */
    const std::string& text() const { return m_text; }

    /**
     * What kind of data an untagged response carries.
     *
     * "FLAGS", "LIST", "LSUB", "SEARCH", "STATUS", "EXISTS", "RECENT",
     * "EXPUNGE", "FETCH", "CAPABILITY", or "" when it carries a condition
     * instead.
     */
    const std::string& name() const { return m_name; }

    /** The number in "* 12 EXISTS" or "* 12 FETCH (...)".  0 when there is none. */
    unsigned long number() const { return m_number; }

    /** The message numbers of a SEARCH. */
    const std::vector<unsigned long>& numbers() const { return m_numbers; }

    /** The flags of a FLAGS, a LIST, or a FETCH's FLAGS attribute. */
    const std::vector<std::string>& flags() const { return m_flags; }

    /** A LIST or LSUB line's hierarchy delimiter, or "" for NIL. */
    const std::string& delimiter() const { return m_delimiter; }

    /** A LIST, LSUB or STATUS line's mailbox name. */
    const std::string& mailbox() const { return m_mailbox; }

    const std::vector<std::string>& capabilities() const { return m_capabilities; }

    /**
     * A FETCH's attributes, by the name they were asked for.
     *
     * The key is the attribute with its section: "RFC822.SIZE", "UID",
     * "BODY[HEADER]".  A string value has its quotes removed and its literal
     * resolved; anything parenthesised -- an ENVELOPE, a BODYSTRUCTURE, an
     * extension nobody here has heard of -- comes back as it was written,
     * because jlib does not interpret those and inventing a representation
     * for them would be worse than handing them over.
     *
     * NIL and the empty string are both "".  RFC 3501 distinguishes them and
     * a std::string cannot; nothing in jlib has needed the difference.
     */
    const std::map<std::string,std::string>& attributes() const { return m_attributes; }

    /** What the server actually sent, literals and all. */
    const std::string& str() const { return m_raw; }

protected:
    kind m_kind = kind::untagged;
    condition m_condition = condition::none;

    std::string m_raw;
    std::string m_tag;
    std::string m_code;
    std::string m_text;
    std::string m_name;
    std::string m_delimiter;
    std::string m_mailbox;

    unsigned long m_number = 0;

    std::vector<unsigned long> m_numbers;
    std::vector<std::string> m_flags;
    std::vector<std::string> m_capabilities;
    std::map<std::string,std::string> m_attributes;

    friend struct reader;
};

}

}
}

#endif // JLIB_NET_IMAP_RESPONSE_HH
