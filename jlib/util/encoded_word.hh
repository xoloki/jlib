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

#ifndef JLIB_UTIL_ENCODED_WORD_HH
#define JLIB_UTIL_ENCODED_WORD_HH

#include <string>
#include <string_view>
#include <vector>

namespace jlib {
namespace util {

/**
 * RFC 2047 encoded words: reading and writing a header that is not ASCII.
 *
 *     rfc2047::decode("=?utf-8?B?U2Now7ZuZW4gVGFn?=")   ->  "Sch\xc3\xb6nen Tag"
 *     rfc2047::encode("Sch\xc3\xb6nen Tag", "utf-8")    ->  "=?utf-8?B?U2Now7ZuZW4=?= Tag"
 *
 * The grammar is in jlib/util/rfc2047.hh.  What is not in the grammar, and is
 * most of the work, is *where* a word may appear: section 5 requires that one
 * be delimited by whitespace or by the punctuation of the construct it sits
 * in, and section 6.2 says whitespace between two adjacent encoded words is
 * not part of the text and disappears when they are decoded.  Both are
 * checked here.
 *
 * ## Charsets are named, not converted
 *
 * decode() returns the octets the sender encoded, in whatever charset they
 * named.  Nothing here transcodes, so a header mixing two charsets decodes to
 * a string that no single charset describes -- which is a property of RFC 2047
 * and not of this implementation.  The two-argument form hands back every
 * charset seen, in order, so a caller that does transcode has what it needs
 * and a caller that does not can at least notice.
 *
 * ## What is not decoded
 *
 * A word that does not parse, is not delimited, or names an encoding other
 * than B or Q is left exactly as it was written.  Showing a user
 * "=?x-unknown?Z?...?=" is worse than showing them the text, but it is much
 * better than showing them a guess.
 */
namespace rfc2047 {

/** Decode every encoded word in a header field value. */
std::string decode(std::string_view s);

/**
 * As decode(), and collects the charset each word named, in order.
 *
 * Empty if the value held no encoded words.  A repeated charset appears once
 * per word, not once per distinct name, so its size is the number of words
 * that were decoded.
 */
std::string decode(std::string_view s, std::vector<std::string>& charsets);

/**
 * Encode the words of s that need it, and leave the rest alone.
 *
 * Splits on whitespace and encodes a *whole* word if any part of it is not
 * ASCII, because section 5 requires an encoded word to be delimited -- the
 * previous implementation here started the encoded word at the first byte
 * over 0x7F, so "Jose Nunez" with a tilde in it produced
 * "Jose N=?utf-8?B?...?=", which no conformant reader will decode.
 *
 * Long runs are split across several words so that none exceeds the 75
 * characters section 2 allows.
 */
std::string encode(std::string_view s, const std::string& charset);

}

}
}

#endif // JLIB_UTIL_ENCODED_WORD_HH
