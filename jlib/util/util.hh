/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_UTIL_HH
#define JLIB_UTIL_HH

#include <sys/stat.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <iomanip>
#include <vector>
#include <list>
#include <iostream>
#include <map>

const int BUF_SIZE=1024;

namespace jlib {
    /**
     * Namespace jlib::util is a collection of static std::string utility functions, 
     * to allow for the use of the std::std::string class while having the
     * convenience of a custom std::string class
     *
     */
	namespace util {

        const std::string ALL_WHITE = "\r\n\t ";
        
        class util_exception : public std::exception {
        public:
            util_exception(const std::string& p_msg = "") {
                m_msg = "util exception: "+p_msg;
            }
            virtual ~util_exception() {}
            virtual const char* what() const noexcept { return m_msg.c_str(); }
        protected:
            std::string m_msg;
        };
      
        /**
         * Convert to upper case.
         *
         * @param s std::string to convert
         * @return s with all characters in upper case
         */
        std::string upper(const std::string& s);

        /**
         * make the first character and every char following a '-' caps
         */
        std::string studly_caps(const std::string& s);
        
        /**
         * Convert to lower case.
         *
         * @param s std::string to convert
         * @return s with all characters in upper case
         */
        std::string lower(const std::string& s);
        
        /**
         * Tokenize this std::string with the given delimiter.
         *
         * @param s std::string to tokenize
         * @param d delimiter for tokenization
         * @return vector of strings
         */

        std::vector<std::string> tokenize(const std::string& s, const std::string& d = " ", bool split_delim = true);

        std::list<std::string> tokenize_list(const std::string& s, const std::string& d = "/", bool split_delim = false);
        
        /**
         * Get a std::string from an int.
         *
         * @param i integer to convert to string
         * @param n desired min length of output (default -1 means don't pad)
         * @return i converted to string
         */
        std::string valueOf(int i, int n=-1);
        std::string string_value(int i, int n=-1);
        
        /**
         * Get a std::string from an unsigned int.
         *
         * @param i unsigned integer to convert to string
         * @param n desired min length of output (default -1 means don't pad)
         * @return i converted to string
         */
        std::string valueOf(unsigned int i, int n=-1);
        std::string string_value(unsigned int i, int n=-1);

        /**
         * Get a std::string from a double.
         *
         * @param i double to convert to string
         * @param n desired min length of output (default -1 means don't pad)
         * @return i converted to string
         */
        std::string valueOf(double i, int n=-1);
        std::string string_value(double i, int n=-1);
        
        /**
         * Get an int from a string.
         *
         * @param s std::string to convert to int.
         * @param base radix to use on conversion
         * @return s converted to int
         */
        int intValue(const std::string& s, int base = 10);
        int int_value(const std::string& s, int base = 10);
        
        /**
         * Get a double from a string.
         *
         * @param s std::string to convert to double
         * @return s converted to double
         */
        double doubleValue(const std::string& s);
        double double_value(const std::string& s);

        std::string hex_value(unsigned char c, bool upper=false);
        std::string hex_value(const std::string& s, bool upper=false);
        std::string hex_value(const unsigned char* data, std::size_t size, bool upper=false, bool space=false);
        
        /**
         * Remove whitespace from beginning of passed string.
         *
         * @param s std::string to chip
         * @return s without leading whitespace
         */
        std::string chip(const std::string& s);
        
        /**
         * Remove whitespace from end of passed string.
         *
         * @param s std::string to chop
         * @return s without trailing whitespace
         */
        std::string chop(const std::string& s);
        
        /**
         * Remove whitespace from beginning and end of passed string.
         *
         * @param s std::string to trim
         * @return s without leading or trailing whitespace
         */
        std::string trim(const std::string& s);
        
        /**
         * Remove characters between passed delimiters
         *
         * @param s std::string to excise
         * @param d1 beginning delimiter
         * @param d2 ending delimiter
         * @return s without d1, d2, or anything between
         */
        std::string excise(const std::string& s, const std::string& d1, const std::string& d2);
        
        /**
         * Remove characters except between passed delimiters
         *
         * @param s std::string to excise
         * @param d1 beginning delimiter
         * @param d2 ending delimiter
         * @return s between d1 and d2, or unchanged if s doesn't contain d1 and d2
         */
        std::string slice(const std::string& s, const std::string& d1, const std::string& d2);
        
        /**
         * Tell if t is a substd::string of s
         *
         * @param s haystack
         * @param t needle
         * @return true if s contains t, ow false
         */
        bool contains(const std::string& s, const std::string& t);
        
        /**
         * Tell if s begins with t.
         *
         * @param s haystack
         * @param t needle
         * @return true if s begins with t, ow false
         */
        bool begins(const std::string& s, const std::string& t);
        
        /**
         * Tell if s ends with t.
         *
         * @param s haystack
         * @param t needle
         * @return true if s ends with t, ow false
         */
        bool ends(const std::string& s, const std::string& t);
        
        /**
         * Tell if t is a substd::string of s
         *
         * @param s haystack
         * @param t needle
         * @return true if s contains t, ow false
         */
        bool icontains(const std::string& s, const std::string& t);
        
        /**
         * Tell if s begins with t.
         *
         * @param s haystack
         * @param t needle
         * @return true if s begins with t, ow false
         */
        bool ibegins(const std::string& s, const std::string& t);
        
        /**
         * Tell if s ends with t.
         *
         * @param s haystack
         * @param t needle
         * @return true if s ends with t, ow false
         */
        bool iends(const std::string& s, const std::string& t);
        
        /**
         * Tell if s equals t, case insensitive.
         *
         * @param s str1
         * @param t str2
         * @return true if s equals t, ow false
         */
        bool iequals(const std::string& s, const std::string& t);

        bool imaps(const std::map<std::string,std::string>& m, const std::string& key, const std::string& val);
        
        // These read a T out of a byte buffer at an arbitrary offset, so the
        // address is not generally aligned for T.  Doing that by casting the
        // pointer and dereferencing is undefined behaviour -- it also breaks
        // strict aliasing -- and -fsanitize=alignment traps it.  memcpy
        // expresses the same thing legally and compiles to the same load.
        template<class T>
        T get(const std::string& s, unsigned int offset=0) {
            T t;
            std::memcpy(&t, s.data() + offset, sizeof(T));
            return t;
        }

        template<class T>
        T get(const char* c, unsigned int offset=0) {
            T t;
            std::memcpy(&t, c + offset, sizeof(T));
            return t;
        }

        template<class T>
        void set(std::string& s, T t, unsigned int offset=0) {
            s.replace(offset, sizeof(t), reinterpret_cast<char*>(&t), sizeof(t));
        }
        
        template<class T>
        void copy(std::string& s, T* t, unsigned int n, unsigned int offset=0) {
            s.replace(offset, (sizeof(T))*(n), reinterpret_cast<char*>(t), (sizeof(T))*(n));
        }
        
        template<class T>
        void byte_copy(std::string& s, T* t, unsigned int n, unsigned int offset=0) {
            s.replace(offset, n, reinterpret_cast<char*>(t), n);
        }

        void load(std::istream& is, std::map<std::string,std::string>& m, bool clear=true);
        void store(std::ostream& os, std::map<std::string,std::string>& m);

        /**
         * base64, RFC 4648 section 4 as MIME uses it.
         *
         * ## Decoding is lenient, and says when it had to be
         *
         * RFC 2045 6.8 requires that characters outside the alphabet be
         * ignored in base64-encoded data -- that rule is what makes the line
         * breaks in a MIME body work, so it is not optional.  But "ignored"
         * covers both a CRLF, which is expected, and a byte of corruption,
         * which is not, so the two-argument form says which happened:
         *
         *     bool clean;
         *     std::string data = base64::decode(part, clean);
         *     if(!clean) { ... }   // something was skipped, or the tail was
         *                          // short, or there was data after the "="
         *
         * A final group of one symbol carries six bits, which is not a byte.
         * It is dropped and clean goes false, rather than being rounded up
         * into a byte that the sender never wrote.
         */
        namespace base64 {

            /** Decoded bytes.  See the note above on what is skipped. */
            std::string decode(const std::string& s);

            /** As decode(s), and clears clean if anything was not as declared. */
            std::string decode(const std::string& s, bool& clean);

            /**
             * Encoded, on one line.
             *
             * No line breaks: an SMTP AUTH token and an RFC 2047 encoded word
             * both go through here and neither may contain whitespace.  Ask
             * for wrapping where a MIME body wants it.
             */
            std::string encode(const std::string& s);

            /** Encoded, broken with CRLF every wrap columns.  0 means never. */
            std::string encode(const std::string& s, std::size_t wrap);

        }

        /**
         * base64url, RFC 4648 section 5, without padding.
         *
         * The same six-bit alphabet with "-" and "_" in place of "+" and "/",
         * so that a value can go in a URL or a filename without being
         * percent-encoded first.
         *
         * **Separate from base64 rather than a flag on it, and the padding is
         * the reason.**  RFC 7636 4.2 requires the PKCE code challenge to be
         * base64url with the "=" stripped, and OAuth2 tokens are compared as
         * whole strings by the server that issued them -- so a challenge with
         * padding on the end simply does not match, and the error a provider
         * returns for it says nothing about padding.  Meanwhile the XOAUTH2
         * blob two files away is standard base64 *with* padding.  Two
         * encodings that differ in three characters, used within a few lines
         * of each other, is exactly the sort of thing a bool argument gets
         * wrong silently.
         *
         * decode() accepts padding if it finds any, because other people's
         * encoders emit it and refusing would be pedantry; encode() never
         * writes it.
         */
        namespace base64url {

            /** Encoded, no padding, no line breaks. */
            std::string encode(const std::string& s);

            /** Decoded.  Padding is tolerated on input and is not required. */
            std::string decode(const std::string& s);

            /** As decode(s), and clears clean if anything was not as declared. */
            std::string decode(const std::string& s, bool& clean);

        }

        /**
         * Quoted-printable, RFC 2045 section 6.7.
         *
         * Not RFC 2047's "Q" encoding, which is a different rule set on the
         * same idea and belongs with Headers.
         */
        namespace qp {

            /**
             * Decoded bytes.
             *
             * An "=" that does not begin a valid escape is passed through as
             * written and clears clean.  It used to be run through strtol,
             * which stops at the first character it cannot read and returns
             * zero, so "=ZZ" became a NUL byte in the middle of the message.
             */
            std::string decode(const std::string& s);
            std::string decode(const std::string& s, bool& clean);

            /** Encoded, with soft line breaks at 76 columns. */
            std::string encode(const std::string& s);

        }

        /**
         * Percent-encoding, RFC 3986 sections 2.1 and 2.3.
         */
        namespace uri {

            /**
             * Everything outside unreserved -- ALPHA DIGIT "-" "." "_" "~" --
             * is escaped.
             *
             * This used to name eleven characters to escape and leave the
             * rest, so a space, a quote and every byte over 0x7F went into a
             * URI untouched.
             */
            std::string encode(const std::string& s);

            /**
             * Decoded bytes.
             *
             * A "%" that does not begin a valid escape is passed through and
             * clears clean.  The old version restarted its search from the
             * front of the string after every replacement, so a "%" that came
             * out of one escape was decoded again -- "%2525" came back as a
             * NUL rather than as "%25".
             */
            std::string decode(const std::string& s);
            std::string decode(const std::string& s, bool& clean);

        }


        namespace xml {
            std::string encode(const std::string& s);
            std::string decode(const std::string& s);
            std::string recode(const std::string& s, const std::map<std::string,std::string>& codec);
        }
       
        namespace file {

            struct stat getstat(const std::string& path);
            long size(const std::string& path);
            long mtime(const std::string& path);
            
            /**
             * slice out from the file at path the regions marked in pts
             * the even indicies are start points, the odds are end points
             * if we have an uneven number, slice out from the last point 
             * to the end
             *
             * the idea here is to kget rid of these regions, and keep the rest
             */
            void kill(const std::string& path, std::vector<long>& pts);

            /**
             * slice out from the file at path the regions marked in pts
             * the even indicies are start points, the odds are end points
             * if we have an uneven number, slice out from the last point 
             * to the end
             *
             * the idea here is to keep these regions, and get rid of the rest
             */
            void keep(const std::string& path, std::vector<long>& pts);

        }
    }
}
#endif
