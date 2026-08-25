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

#ifndef JLIB_NET_CONTENT_TYPE_HH
#define JLIB_NET_CONTENT_TYPE_HH

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace jlib {
namespace net {

/**
 * A Content-Type or Content-Disposition, parsed against MIME's grammar.
 *
 * The grammar is RFC 2045 5.1 and RFC 2231, in jlib/net/rfc2045.hh, read by
 * jlib::util::abnf::compile().  Nothing here scans by hand.
 *
 *     content_type c = content_type::parse(email.find("CONTENT-TYPE"));
 *     c.is("multipart")        ->  true for multipart/mixed, and only for that
 *     c.get("boundary")        ->  the boundary, unquoted
 *
 * ## Why a header this simple needs a grammar
 *
 *     Content-Type: multipart/mixed; boundary="a;b"
 *
 * That boundary is legal, and every parser that splits the header on ";" and
 * takes what is between the first quote and the last gets it wrong -- which is
 * what jlib did, and what gtkmail then did again on top of it.  The failure is
 * not cosmetic: a wrong boundary means the message does not split into its
 * parts, so an attachment is not found or half the body is shown as an
 * attachment.
 *
 * The same header also has to survive a comment ("text/plain (why not)"),
 * whitespace around the "=", a missing or repeated semicolon, and an unquoted
 * value that the old code's util::slice would have handed back whole.
 *
 * ## RFC 2231
 *
 * Parameters may be split across numbered sections and may carry a charset:
 *
 *     Content-Disposition: attachment;
 *       filename*0*=UTF-8''%E2%98%83;
 *       filename*1=.txt
 *
 * The sections are joined in order, the percent-escapes decoded, and the
 * charset kept:
 *
 *     c.get("filename")        ->  the octets of "☃.txt"
 *     c.charset_of("filename") ->  "UTF-8"
 *
 * They are handed back as octets rather than converted, and the charset is
 * exposed so a caller *can* convert.  jlib does not transcode, and silently
 * returning bytes in an unnamed encoding is how the rest of this library's
 * charset handling went wrong.
 *
 * ## Case
 *
 * Type, subtype and parameter names are lowercased on the way in, because
 * RFC 2045 5.1 says they are case insensitive.  Parameter *values* are not:
 * a boundary is case sensitive and so is a filename.
 */
class content_type {
public:
    /** What could not be read, and where it stopped making sense. */
    class exception : public std::runtime_error {
    public:
        exception(const std::string& msg, std::string text, std::size_t offset);

        const std::string& text() const { return m_text; }
        std::size_t offset() const { return m_offset; }

    protected:
        std::string m_text;
        std::size_t m_offset;
    };

    /**
     * One parameter.
     *
     * A struct rather than a pair because charset is not optional information
     * once RFC 2231 is in play -- it is the difference between a filename and
     * a run of octets.
     */
    struct parameter {
        std::string name;      ///< lowercased
        std::string value;     ///< unquoted, unescaped, sections joined
        std::string charset;   ///< "" unless RFC 2231 named one
    };

    typedef std::vector<parameter> parameter_list;

    content_type() = default;

    /** "type/subtype; a=b".  Throws content_type::exception. */
    static content_type parse(std::string_view s);

    /**
     * "attachment; filename=x", RFC 2183.
     *
     * Same grammar with no subtype, because RFC 2183 2 says so: the
     * disposition type uses "the parameter syntax of RFC 2045".  subtype() is
     * empty for one of these.
     */
    static content_type parse_disposition(std::string_view s);

    /** Would it parse?  The way to ask without catching. */
    static bool valid(std::string_view s);

    const std::string& type() const { return m_type; }
    const std::string& subtype() const { return m_subtype; }

    /** "text/plain", or just the disposition type when there is no subtype. */
    std::string essence() const;

    /**
     * Is this that type?
     *
     * A whole-component comparison, not a substring search.  Email::is("text")
     * used to be find() != npos over the entire header, so it was true for
     * application/x-latext and true for any multipart whose boundary happened
     * to contain the letters "text".
     */
    bool is(std::string_view type) const;
    bool is(std::string_view type, std::string_view subtype) const;

    bool has(std::string_view name) const;

    /** The parameter's value, or fallback when it is not there. */
    std::string get(std::string_view name,
                    const std::string& fallback = std::string()) const;

    /** What charset get(name) is in, or "" when nothing said. */
    std::string charset_of(std::string_view name) const;

    const parameter_list& parameters() const { return m_parameters; }

    /** Set the parameter of this name, adding it if it is not there. */
    void set(std::string name, std::string value);

    /** Canonical, requoting any value that needs it. */
    std::string str() const;

protected:
    std::string m_type;
    std::string m_subtype;
    parameter_list m_parameters;

    friend struct mime_reader;
};

/**
 * The parts of a multipart body, per RFC 2046 5.1.1.
 *
 * The delimiter is CRLF "--" boundary -- the line break belongs to the
 * delimiter and not to the part before it, which is the detail that decides
 * whether every part comes back with a stray blank line on the end.  Transport
 * padding after the boundary is skipped, the preamble before the first
 * delimiter and the epilogue after the closing "--" are discarded, and a body
 * with no delimiter in it at all comes back empty rather than as one part.
 *
 * A bare LF is accepted where the RFC says CRLF, because a message that has
 * been through a file, an mbox, or anything else that normalises line endings
 * no longer has its CRLFs and is not thereby a different message.
 */
std::vector<std::string> split_multipart(std::string_view body,
                                         std::string_view boundary);

}
}

#endif // JLIB_NET_CONTENT_TYPE_HH
