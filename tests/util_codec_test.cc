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

// base64, quoted-printable and percent-encoding.
//
// These three sit under every MIME body and every encoded header, so they read
// bytes a stranger chose.  Two of the three were reading out of bounds while
// doing it, and all three answered a malformed escape with a NUL byte.

#include <jlib/util/util.hh>

#include <iostream>
#include <string>

using namespace jlib::util;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static std::string show(const std::string& s) {
    std::string out;

    for(unsigned char c : s) {
        if(c == '\\')                       out += "\\\\";
        else if(c == '\r')                  out += "\\r";
        else if(c == '\n')                  out += "\\n";
        else if(c >= 0x20 && c < 0x7f)      out += static_cast<char>(c);
        else {
            static const char* h = "0123456789abcdef";
            out += "\\x";
            out += h[c >> 4];
            out += h[c & 0xF];
        }
    }

    return out;
}

/** Every byte value, so nothing can be right by accident on ASCII alone. */
static std::string all_octets() {
    std::string s;

    for(int i = 0; i < 256; i++) s += static_cast<char>(i);

    return s;
}

static void base64_round_trips() {
    std::cout << "base64, RFC 4648 4:\n";

    // RFC 4648 section 10's vectors, which is the point of having them.
    struct { const char* in; const char* out; } v[] = {
        { "",       ""         },
        { "f",      "Zg=="     },
        { "fo",     "Zm8="     },
        { "foo",    "Zm9v"     },
        { "foob",   "Zm9vYg==" },
        { "fooba",  "Zm9vYmE=" },
        { "foobar", "Zm9vYmFy" },
    };

    for(const auto& t : v) {
        ok(std::string("encode(\"") + t.in + "\")",
           base64::encode(t.in) == t.out, base64::encode(t.in));
        ok(std::string("decode(\"") + t.out + "\")",
           base64::decode(t.out) == t.in, show(base64::decode(t.out)));
    }

    // And every byte, at every alignment, so a sign-extension bug in the
    // encoder cannot hide in the bytes ASCII does not reach.
    for(int off = 0; off < 3; off++) {
        const std::string in = std::string(off, 'x') + all_octets();

        ok("all 256 octets round trip at offset " + std::to_string(off),
           base64::decode(base64::encode(in)) == in);
    }
}

static void base64_reads_within_its_input() {
    std::cout << "\nbase64 on input from a stranger:\n";

    // The decode table had 123 entries and was indexed with a plain char, so
    // "{|}~" ran off the end of it and every byte over 0x7F was a negative
    // index.  ASan caught this one; the test is here so it stays caught.
    bool clean = true;

    ok("\"~~~~\" does not read past the table",
       base64::decode("~~~~", clean).empty());
    ok("  and says the input was not base64", !clean);

    ok("a byte over 0x7F is skipped, not indexed",
       base64::decode("\xff\xfe\xfd\xfc", clean).empty() && !clean);

    // The old decoder stepped four at a time and read in[2] and in[3]
    // unconditionally, so a final group of one to three symbols read past the
    // end of the string and appended whatever was there.  That is what a
    // truncated message looks like.
    ok("a two-symbol tail decodes to one byte",
       base64::decode("aGk") == "hi", show(base64::decode("aGk")));

    ok("and a long one does not pick up a neighbour",
       base64::decode("aGVsbG8gdGhlcmUgd29ybGQhIQ") == "hello there world!!",
       show(base64::decode("aGVsbG8gdGhlcmUgd29ybGQhIQ")));

    // Two symbols legitimately encode one byte -- 12 bits, of which 8 are
    // used -- so a short tail is not by itself wrong.
    ok("a two-symbol tail is a byte", base64::decode("Zm9vYg") == "foob",
       show(base64::decode("Zm9vYg")));

    // One symbol is six bits, which is not a byte at any alignment.
    ok("a one-symbol tail is dropped, not rounded up",
       base64::decode("Zm9vY", clean) == "foo" && !clean,
       show(base64::decode("Zm9vY")));
}

static void base64_is_lenient_and_says_so() {
    std::cout << "\nbase64, what was skipped:\n";

    bool clean = false;

    // RFC 2045 6.8 requires this: it is what makes a wrapped MIME body work.
    ok("line breaks are ignored",
       base64::decode("Zm9v\r\nYmFy", clean) == "foobar");
    ok("  and that is not a complaint", clean);

    ok("but a character that is not whitespace is",
       base64::decode("Zm9v!YmFy", clean) == "foobar" && !clean);

    // Padding means the data ended.  Decoding on past it produced a byte
    // that neither half of "Zg==Zg==" encodes.
    ok("decoding stops at the padding",
       base64::decode("Zg==Zg==", clean) == "f" && !clean,
       show(base64::decode("Zg==Zg==")));
}

static void base64_line_wrapping() {
    std::cout << "\nbase64 line breaks:\n";

    const std::string long_input(200, 'x');

    // The old encoder wrapped at 64 unconditionally.  Two of the three callers
    // are broken by that: an SMTP AUTH token with a newline in it is not a
    // valid command, and RFC 2047 2 says an encoded word may contain no
    // whitespace at all.  A credential over 48 bytes was long enough to hit it.
    const std::string plain = base64::encode(long_input);

    ok("encode() does not wrap",
       plain.find('\n') == std::string::npos && plain.find('\r') == std::string::npos);

    ok("an SMTP AUTH token stays on one line",
       base64::encode(std::string(1, '\0') + std::string(40, 'u') +
                      std::string(1, '\0') + std::string(40, 'p'))
           .find('\n') == std::string::npos);

    const std::string wrapped = base64::encode(long_input, 76);

    ok("encode(s, 76) does wrap", wrapped.find("\r\n") != std::string::npos);

    for(std::string::size_type i = 0, n; i < wrapped.size(); i = n + 2) {
        n = wrapped.find("\r\n", i);
        const std::string line = wrapped.substr(i, n == wrapped.npos ? n : n - i);

        if(line.size() > 76) {
            ok("no line over 76 characters", false, std::to_string(line.size()));
            break;
        }

        if(n == wrapped.npos) break;
    }

    ok("no line over 76 characters", true);
    ok("and it still decodes to the same bytes",
       base64::decode(wrapped) == long_input);
}

static void quoted_printable() {
    std::cout << "\nquoted-printable, RFC 2045 6.7:\n";

    ok("a plain string is itself", qp::decode("hello") == "hello");
    ok("an escape",                qp::decode("a=3Db") == "a=b");
    ok("lowercase hex too",        qp::decode("=e2=98=83") == "\xe2\x98\x83");
    ok("a soft line break",        qp::decode("abc=\r\ndef") == "abcdef");
    ok("and a bare LF one",        qp::decode("abc=\ndef") == "abcdef");

    bool clean = true;

    // strtol stops at the first character it cannot read and returns zero, so
    // every one of these used to come back as a NUL byte in the middle of the
    // message.
    struct { const char* in; const char* out; } bad[] = {
        { "=ZZ", "=ZZ" },
        { "a=",  "a="  },
        { "=1",  "=1"  },
        { "= 1", "= 1" },
        { "=+1", "=+1" },
    };

    for(const auto& b : bad) {
        clean = true;
        const std::string got = qp::decode(b.in, clean);

        ok(std::string("\"") + b.in + "\" is passed through, not zeroed",
           got == b.out && !clean, show(got));
    }

    // encode() was an empty function returning "".
    ok("encode is not a stub any more", qp::encode("hello") == "hello");
    ok("an equals sign is escaped",     qp::encode("a=b") == "a=3Db");
    ok("and a high byte",               qp::encode("\xe2\x98\x83") == "=E2=98=83",
       qp::encode("\xe2\x98\x83"));

    // Rule 3: whitespace may stand for itself, but not where a transport would
    // strip it.
    ok("a space in the middle stays a space", qp::encode("a b") == "a b");
    ok("a trailing one is escaped",           qp::encode("a ") == "a=20",
       qp::encode("a "));

    for(int off = 0; off < 3; off++) {
        const std::string in = std::string(off, 'x') + all_octets();

        ok("all 256 octets round trip at offset " + std::to_string(off),
           qp::decode(qp::encode(in)) == in);
    }

    // Rule 5.
    const std::string wrapped = qp::encode(std::string(300, 'x'));

    bool fits = true;

    for(std::string::size_type i = 0, n; i < wrapped.size(); i = n + 2) {
        n = wrapped.find("\r\n", i);
        if(n == wrapped.npos) break;
        if(n - i > 76) fits = false;
    }

    ok("no encoded line runs past 76 characters", fits);
}

static void percent_encoding() {
    std::cout << "\npercent-encoding, RFC 3986 2.1 and 2.3:\n";

    // The decoder restarted its search from the front of the string after
    // every replacement, so a "%" that came out of one escape was decoded
    // again -- and once there was nothing left to read, strtol returned zero.
    ok("a percent that decodes to a percent stops there",
       uri::decode("%2525") == "%25", show(uri::decode("%2525")));

    bool clean = true;

    ok("a truncated escape is passed through",
       uri::decode("a%", clean) == "a%" && !clean, show(uri::decode("a%")));
    ok("and one that is not hex",
       uri::decode("%zz", clean) == "%zz" && !clean, show(uri::decode("%zz")));

    ok("a good one is not a complaint",
       uri::decode("a%20b", clean) == "a b" && clean);

    // encode used to name eleven characters to escape and leave everything
    // else, so a space went into a URI as a space.
    ok("a space is escaped",   uri::encode("a b") == "a%20b", uri::encode("a b"));
    ok("a quote is escaped",   uri::encode("\"") == "%22");
    ok("a high byte is too",   uri::encode("\xe2\x98\x83") == "%E2%98%83",
       uri::encode("\xe2\x98\x83"));

    // RFC 3986 2.3: these six and only these six stand for themselves.
    ok("unreserved characters are left alone",
       uri::encode("aZ09-._~") == "aZ09-._~", uri::encode("aZ09-._~"));

    ok("uppercase hex, which 2.1 calls canonical",
       uri::encode("\xff") == "%FF", uri::encode("\xff"));

    for(int off = 0; off < 3; off++) {
        const std::string in = std::string(off, 'x') + all_octets();

        ok("all 256 octets round trip at offset " + std::to_string(off),
           uri::decode(uri::encode(in)) == in);
    }
}

int main() {
    std::cout << std::unitbuf;

    base64_round_trips();
    base64_reads_within_its_input();
    base64_is_lenient_and_says_so();
    base64_line_wrapping();
    quoted_printable();
    percent_encoding();

    // What a green run does NOT establish.
    //
    // Not memory safety.  A test can show that a decoder returns the right
    // bytes; it cannot show that it read only its own.  The out-of-bounds
    // reads these tests are named for were found by AddressSanitizer, not by
    // assertions, and nothing in `make check` runs under it -- so a future
    // regression of the same kind would produce the right answer here and
    // still be a bug.  Build with -fsanitize=address to check that part.
    //
    // Not RFC 2047's "Q" encoding.  qp::encode is the body form of
    // quoted-printable; the header form writes a space as "_" and must escape
    // everything a field may not contain.  Headers::encode still only ever
    // emits "?B?", which is why nobody noticed qp::encode was an empty stub.
    //
    // Not the base64 alphabet variants.  RFC 4648 section 5's URL-safe
    // alphabet, which uses "-" and "_" for 62 and 63, is not accepted and
    // decodes to nothing with clean set false.  Nothing in jlib needs it.
    //
    // Not canonical-form checking.  RFC 4648 section 3.5 allows an
    // implementation to reject a final group whose unused bits are not zero;
    // this one accepts it silently, because real mail contains it.
    return failures ? 1 : 0;
}
