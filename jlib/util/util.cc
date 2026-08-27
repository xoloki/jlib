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

#include <jlib/sys/tfstream.hh>
#include <jlib/sys/sync.hh>
#include <jlib/sys/sys.hh>

#include <jlib/util/util.hh>

#include <algorithm>
#include <sstream>

#include <sys/types.h>
#include <sys/stat.h>

const std::string WHITESPACE = "\n\r\f\t ";
const int SZ = 64;

namespace jlib {
	namespace util {

        std::string studly_caps(const std::string& s) {
            std::string ret = lower(s);
            if(s.length() > 0) {
                ret[0] = toupper(ret[0]);

                std::string::size_type i=0, j;

                while( (j=ret.find("-",i)) != ret.npos) {
                    if(j+1 < ret.length()) {
                        ret[j+1] = toupper(ret[j+1]);
                        i = j+1;
                    }
                }
            }
            return ret;
        }


        std::vector<std::string> tokenize(const std::string& s, const std::string& d, bool split_delim) {
            std::vector<std::string> ret;
            
            std::string::size_type i=0, j;

            if(!split_delim) {
                while(s.find(d,i) == i) i += d.length();
            }

            while( (j=s.find(d,i)) != std::string::npos ) {
                ret.push_back(s.substr(i, j - i));
                i = j + d.length();
                if(!split_delim) {
                    while(s.find(d, i) == i) i += d.length();
                }
            }

            ret.push_back(s.substr(i));

            return ret;
        }

        std::list<std::string> tokenize_list(const std::string& s, const std::string& d, bool split_delim) {
            std::list<std::string> ret;
            
            std::string::size_type i=0, j;
            if(!split_delim) {
                while(s.find(d,i) == i) i += d.length();
            }
            while( (j=s.find(d,i)) != s.npos ) {
                ret.push_back(s.substr(i, j-i));
                i=j+d.length();
                if(!split_delim) {
                    while(s.find(d,i) == i) i += d.length();
                }
            }
            
            if(split_delim || s.substr(i) != "")
                ret.push_back(s.substr(i));

            return ret;
        }

        std::string excise(const std::string& s, const std::string& d1, const std::string& d2) {
            std::string ret = s;

            // These were ints compared against -1.  find() returns a
            // size_type, so npos survived only as an implementation-defined
            // narrowing that happens to give -1 -- and stops doing so the
            // moment the string is longer than INT_MAX.
            std::string::size_type i, j = 0;

            while( (i=ret.find(d1,j)) != ret.npos && (j=ret.find(d2,i+1)) != ret.npos ) {
                ret.erase(i,j+1-i);
                j = i;
                //cout << "Excising '" << ret.c_str() << "' between '" << d1.c_str() << "' and '" << d2.c_str() << "' = '" << ret.c_str() << "'\n";
            }
            
            return ret;
        }
        
        std::string slice(const std::string& s, const std::string& d1, const std::string& d2) {
            std::string::size_type i, j;
            
            if( (i=s.find(d1)) != s.npos && (j=s.find(d2,i+1)) != s.npos ) {
                return s.substr(i+d1.size(),j-(i+d1.size()));
            }
            else {
                return s;
            }
        }
        
        std::string chip(const std::string& s) {
            std::string::size_type i = s.find_first_not_of(WHITESPACE);
            if(i != s.npos) {
                return s.substr(i);
            }
            else {
                return "";
            }
        }
        
        std::string chop(const std::string& s) {
            std::string::size_type i = s.find_last_not_of(WHITESPACE);
            if(i != s.npos) {
                return s.substr(0,i+1);
            }
            else {
                return "";
            }
        }
        
        std::string trim(const std::string& s) {
            return chip(chop(s));
        }
        
        void load(std::istream& is, std::map<std::string,std::string>& m, bool clear) {
            if(clear) m.clear();
            std::string buf, key, val;
            while(!is.eof()) {
                jlib::sys::getline(is,buf);
                buf = buf.substr(0,buf.find("#"));
                std::string::size_type p;
                if( (p=buf.find("=")) != buf.npos ) {
                    key = trim(buf.substr(0,p));
                    val = trim(buf.substr(p+1));
                    m[key]=val;
                }
            }
        }
        
        void store(std::ostream& os, std::map<std::string,std::string>& m) {
            std::map<std::string,std::string>::iterator i = m.begin();
            while(i != m.end()) {
                os << i->first << " = " << i->second << std::endl;
                i++;
            }
        }

        bool imaps(const std::map<std::string,std::string>& m, const std::string& key, const std::string& val) {
            return (upper(const_cast< std::map<std::string,std::string>& >(m)[key]) == upper(val));
        }
        
        std::string upper(const std::string& s) {
            std::string ret = s;
            for(std::string::size_type i=0; i<ret.size(); i++) {
                ret[i] = toupper(ret[i]);
            }
            return ret;
        }
        
        std::string lower(const std::string& s) {
            std::string ret = s;
            for(std::string::size_type i=0; i<ret.size(); i++) {
                ret[i] = tolower(ret[i]);
            }
            return ret;
        }
        
        /*
         * Unfortunately, ostrstreams aren't working correctly here
         * I'll stick to the C style implementation until I get
         * them working, or until gcc3 comes out with real stringstreams
         */

        std::string string_value(int i, int n) { return valueOf(i,n); }
        std::string valueOf(int i, int n) {
            char* fmt = new char[SZ];
            char* buf = new char[SZ];

            if(n == -1) 
                snprintf(fmt,SZ-1,"%%d");
            else
                snprintf(fmt,SZ-1,"%%0%dd",n);
            snprintf(buf,SZ-1,fmt,i);

            std::string ret(buf);
            delete [] fmt;
            delete [] buf;
            return ret;
        }
        
        std::string string_value(unsigned int i, int n) { return valueOf(i,n); }
        std::string valueOf(unsigned int i, int n) {
            char* fmt = new char[SZ];
            char* buf = new char[SZ];

            if(n == -1) 
                snprintf(fmt,SZ-1,"%%u");
            else
                snprintf(fmt,SZ-1,"%%0%du",n);
            snprintf(buf,SZ-1,fmt,i);

            std::string ret(buf);
            delete [] fmt;
            delete [] buf;
            return ret;
        }
        
        std::string string_value(double i, int n) { return valueOf(i,n); }
        std::string valueOf(double i, int n) {
            char* fmt = new char[SZ];
            char* buf = new char[SZ];

            if(n == -1) 
                snprintf(fmt,SZ-1,"%%f");
            else
                snprintf(fmt,SZ-1,"%%0%df",n);
            snprintf(buf,SZ-1,fmt,i);

            std::string ret(buf);
            delete [] fmt;
            delete [] buf;
            return ret;
        }


        /*
        std::string valueOf(int i, int n) {
            ostrstream os;
            if(n == -1) 
                os << i << ends;
            else
                os << setw(n) << setfill('0') << i << ends;
            std::string ret(os.str());
            delete [] os.str();
            return ret;
        }
        
        std::string valueOf(unsigned int i, int n) {
            ostrstream os;
            if(n == -1) 
                os << i << ends;
            else
                os << setw(n) << setfill('0') << i << ends;
            std::string ret = os.str();
            delete  [] os.str();
            return ret;
        }
        
        std::string valueOf(double i, int n) {
            ostrstream os;
            if(n == -1) 
                os << i << ends;
            else
                os << setw(n) << setfill('0') << i << ends;
            std::string ret = os.str();
            delete [] os.str();
            return ret;
        }
        */
        
        int int_value(const std::string& s, int base) { return intValue(s,base); }
        int intValue(const std::string& s, int base) {
            return strtol(s.c_str(), NULL, base);
        }
        
        double double_value(const std::string& s) { return doubleValue(s); }
        double doubleValue(const std::string& s) {
            return strtod(s.c_str(), NULL);
        }
        
        std::string hex_value(unsigned char c, bool upper) {
            /*
            std::ostringstream o;
            o << std::hex << c;
            return o.str();
            */

            const unsigned int size=16;
            char buffer[size];

            snprintf(buffer, size-1, "%02x", c);

            std::string ret(buffer);
            
            if(upper) {
                return jlib::util::upper(ret);
            }
            else {
                return ret;
            }

        }

        std::string hex_value(const std::string& s, bool upper) {
            std::string ret;
            for(std::string::size_type i=0;i<s.length();i++) {
                ret += hex_value(static_cast<unsigned char>(s[i]));
                if(i+1 < s.length())
                    ret += " ";
            }
            return ret;
        }

        std::string hex_value(const unsigned char* data, std::size_t size, bool upper, bool space) {
            std::string ret;
            for(std::size_t i = 0; i < size; i++) {
                ret += hex_value(data[i]);
                if(space && (i + 1) < size)
                    ret += " ";
            }
            return ret;
        }

        bool contains(const std::string& s, const std::string& t) {
            return (s.size() >= t.size() && (int)s.find(t) != -1);
        }
        
        bool begins(const std::string& s, const std::string& t) {
            return (s.size() >= t.size() && s.find(t) == 0);
        }
        
        bool ends(const std::string& s, const std::string& t) {
            return ( s.size() >= t.size() && s.rfind(t) == (s.size()-t.size()) );
        }
        
        bool icontains(const std::string& s, const std::string& t) {
            // The cast to int truncated npos; see excise() above.
            return (s.size() >= t.size() && upper(s).find(upper(t)) != std::string::npos);
        }
        
        bool ibegins(const std::string& s, const std::string& t) {
            return (s.size() >= t.size() && upper(s).find(upper(t)) == 0);
        }
        
        bool iends(const std::string& s, const std::string& t) {
            return ( s.size() >= t.size() && upper(s).rfind(upper(t)) == (s.size()-t.size()) );
        }
        
        bool iequals(const std::string& s, const std::string& t) {
            return ( upper(s) == upper(t) );
        }        

        // ------------------------------------------------------------ base64

        namespace base64 {

            static const char* const ALPHABET =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

            /**
             * 256 entries, indexed through unsigned char.
             *
             * The old table had 123 and was indexed with a plain char, so
             * every byte over 0x7F was a negative index and "{|}~" ran off
             * the end.  Both are out-of-bounds reads on input that arrives
             * from the network -- ASan caught the second one on
             * decode("~~~~").
             */
            static const signed char* table()
            {
                static signed char t[256];
                static const bool built = [] {
                    for(int i = 0; i < 256; i++) t[i] = -1;
                    for(int i = 0; i < 64; i++) {
                        t[static_cast<unsigned char>(ALPHABET[i])] =
                            static_cast<signed char>(i);
                    }

                    return true;
                }();

                (void)built;

                return t;
            }

            std::string decode(const std::string& s, bool& clean)
            {
                std::string out;

                out.reserve(s.size() / 4 * 3);
                clean = true;

                // Three decoded bytes per four symbols, accumulated six bits
                // at a time.  The old version stepped four bytes at a time and
                // read in[2] and in[3] unconditionally, so a final group of
                // one to three symbols -- which is what a truncated message
                // or a stripped line break leaves -- read past the end of the
                // string and appended whatever was there.
                unsigned int bits = 0;
                int have = 0;
                bool padded = false;

                for(unsigned char c : s) {
                    if(c == '=') { padded = true; continue; }

                    const signed char v = table()[c];

                    if(v < 0) {
                        // RFC 2045 6.8: characters outside the alphabet are
                        // ignored in base64-encoded data.  That rule is what
                        // makes the line breaks in a MIME body work, so it is
                        // not optional -- but anything other than whitespace
                        // being skipped means the input was not what it said.
                        if(c != '\r' && c != '\n' && c != ' ' && c != '\t') {
                            clean = false;
                        }

                        continue;
                    }

                    // Padding means the data ended.  Whatever follows it is
                    // not part of this value, and decoding it anyway is how
                    // "Zg==Zg==" produced a byte that neither half encodes.
                    if(padded) { clean = false; break; }

                    bits = (bits << 6) | static_cast<unsigned int>(v);
                    have += 6;

                    if(have >= 8) {
                        have -= 8;
                        out += static_cast<char>((bits >> have) & 0xFF);
                    }
                }

                // Six leftover bits cannot be a byte.  They are dropped rather
                // than rounded up into one: inventing a byte is how the old
                // version turned "aGk" into "hi" plus a stray character.
                if(have >= 6) clean = false;

                return out;
            }

            std::string decode(const std::string& s)
            {
                bool clean;

                return decode(s, clean);
            }

            std::string encode(const std::string& s, std::size_t wrap)
            {
                std::string out;

                out.reserve((s.size() + 2) / 3 * 4 + (wrap ? s.size() / wrap : 0));

                std::size_t column = 0;

                for(std::size_t i = 0; i < s.size(); i += 3) {
                    const std::size_t n = std::min<std::size_t>(3, s.size() - i);

                    unsigned int bits = 0;

                    for(std::size_t k = 0; k < 3; k++) {
                        bits = (bits << 8) |
                               (k < n ? static_cast<unsigned char>(s[i + k]) : 0);
                    }

                    char quad[4];

                    for(int k = 0; k < 4; k++) {
                        quad[k] = ALPHABET[(bits >> (18 - 6 * k)) & 0x3F];
                    }

                    // One padding character per byte the last group was short.
                    for(std::size_t k = n + 1; k < 4; k++) quad[k] = '=';

                    if(wrap && column + 4 > wrap) {
                        out += "\r\n";
                        column = 0;
                    }

                    out.append(quad, 4);
                    column += 4;
                }

                return out;
            }

            std::string encode(const std::string& s)
            {
                // No wrapping by default, which is a change.  The old version
                // put a newline in after every 64 characters, unconditionally
                // -- including in the middle of an SMTP AUTH token, which
                // breaks the command, and in the middle of an RFC 2047
                // encoded word, which may not contain whitespace at all.  The
                // callers that want a wrapped MIME body ask for one.
                return encode(s, 0);
            }

        }

        // --------------------------------------------------------- base64url

        namespace base64url {

            std::string encode(const std::string& s)
            {
                std::string out = base64::encode(s);

                // The three characters that differ, and the padding.  RFC 4648
                // 5 is the same alphabet with two substitutions; RFC 7636 4.2
                // is what requires the "=" to come off.
                for(char& c : out) {
                    if(c == '+')      c = '-';
                    else if(c == '/') c = '_';
                }

                while(!out.empty() && out.back() == '=') out.pop_back();

                return out;
            }

            std::string decode(const std::string& s, bool& clean)
            {
                std::string in;

                in.reserve(s.size() + 3);

                for(char c : s) {
                    if(c == '-')      in += '+';
                    else if(c == '_') in += '/';
                    else if(c == '=') continue;   // tolerated, not required
                    else              in += c;
                }

                // base64::decode drops a final group of one symbol and clears
                // clean, which is the right answer -- six bits is not a byte.
                // Padding it back is only so the length is a multiple of four,
                // which is what that decoder expects to see.
                while(in.size() % 4) in += '=';

                return base64::decode(in, clean);
            }

            std::string decode(const std::string& s)
            {
                bool clean = true;

                return decode(s, clean);
            }

        }

        // -------------------------------------------------- quoted-printable

        namespace qp {

            static int hex(unsigned char c)
            {
                if(c >= '0' && c <= '9') return c - '0';
                if(c >= 'a' && c <= 'f') return c - 'a' + 10;
                if(c >= 'A' && c <= 'F') return c - 'A' + 10;

                return -1;
            }

            std::string decode(const std::string& s, bool& clean)
            {
                std::string out;

                out.reserve(s.size());
                clean = true;

                for(std::string::size_type i = 0; i < s.size(); i++) {
                    if(s[i] != '=') { out += s[i]; continue; }

                    // A soft line break, RFC 2045 6.7 rule 5.
                    if(i + 2 < s.size() && s[i + 1] == '\r' && s[i + 2] == '\n') {
                        i += 2;
                        continue;
                    }

                    if(i + 1 < s.size() && s[i + 1] == '\n') {
                        i += 1;
                        continue;
                    }

                    const int hi = i + 1 < s.size() ? hex(s[i + 1]) : -1;
                    const int lo = i + 2 < s.size() ? hex(s[i + 2]) : -1;

                    // An escape that is not one.  Passed through as written
                    // rather than decoded: strtol used to accept "=ZZ", stop
                    // at the first character it could not read, and return
                    // zero, so a stray equals sign became a NUL byte in the
                    // middle of the message.
                    if(hi < 0 || lo < 0) {
                        clean = false;
                        out += s[i];
                        continue;
                    }

                    out += static_cast<char>(hi * 16 + lo);
                    i += 2;
                }

                return out;
            }

            std::string decode(const std::string& s)
            {
                bool clean;

                return decode(s, clean);
            }

            std::string encode(const std::string& s)
            {
                // RFC 2045 6.7, the body form.  This was an empty function
                // returning the empty string, with a TODO where the body
                // should be.
                //
                // Not RFC 2047's "Q" encoding, which is a different rule set
                // on the same idea -- it writes a space as "_" and must escape
                // anything a header field may not contain.  That belongs with
                // Headers::encode.
                static const char* const HEX = "0123456789ABCDEF";

                std::string out;
                std::size_t column = 0;

                for(std::string::size_type i = 0; i < s.size(); i++) {
                    const unsigned char c = static_cast<unsigned char>(s[i]);

                    if(c == '\n') {
                        out += '\n';
                        column = 0;
                        continue;
                    }

                    // Rule 3: whitespace may stand for itself, but not at the
                    // end of a line, where transport would strip it.
                    const bool trailing = (c == ' ' || c == '\t') &&
                        (i + 1 == s.size() || s[i + 1] == '\n' || s[i + 1] == '\r');

                    const bool literal = c >= 33 && c <= 126 && c != '=';
                    const std::size_t width = (literal || (!trailing &&
                                              (c == ' ' || c == '\t'))) ? 1 : 3;

                    // Rule 5: at most 76 characters, and the "=" of a soft
                    // break needs a column of its own.
                    if(column + width > 75) {
                        out += "=\r\n";
                        column = 0;
                    }

                    if(width == 1) {
                        out += static_cast<char>(c);
                    }
                    else if(c == '\r') {
                        // A bare CR, since CRLF was handled above.
                        out += "=0D";
                    }
                    else {
                        out += '=';
                        out += HEX[c >> 4];
                        out += HEX[c & 0x0F];
                    }

                    column += width;
                }

                return out;
            }

        }

        // --------------------------------------------------------------- uri

        namespace uri {

            /**
             * RFC 3986 2.3.  Everything else is escaped.
             *
             * The old list went the other way round -- it named eleven
             * characters to escape and left the rest alone -- so a space, a
             * quote and every byte over 0x7F went into a URI untouched.
             */
            static bool unreserved(unsigned char c)
            {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') ||
                       c == '-' || c == '.' || c == '_' || c == '~';
            }

            std::string encode(const std::string& s)
            {
                static const char* const HEX = "0123456789ABCDEF";

                std::string out;

                out.reserve(s.size());

                for(unsigned char c : s) {
                    if(unreserved(c)) {
                        out += static_cast<char>(c);
                    }
                    else {
                        // 2.1: uppercase hex digits are the canonical form.
                        out += '%';
                        out += HEX[c >> 4];
                        out += HEX[c & 0x0F];
                    }
                }

                return out;
            }

            std::string decode(const std::string& s, bool& clean)
            {
                std::string out;

                out.reserve(s.size());
                clean = true;

                for(std::string::size_type i = 0; i < s.size(); i++) {
                    if(s[i] != '%') { out += s[i]; continue; }

                    const int hi = i + 1 < s.size() ? qp::hex(s[i + 1]) : -1;
                    const int lo = i + 2 < s.size() ? qp::hex(s[i + 2]) : -1;

                    if(hi < 0 || lo < 0) {
                        clean = false;
                        out += s[i];
                        continue;
                    }

                    out += static_cast<char>(hi * 16 + lo);
                    i += 2;
                }

                return out;
            }

            std::string decode(const std::string& s)
            {
                bool clean;

                return decode(s, clean);
            }

        }

        namespace xml {

            std::string encode(const std::string& s) {
                //static jlib::sys::sync< std::map< std::string, std::string> > encmap;
                static jlib::sys::sync< std::map< std::string, std::string> > encmap;

                encmap.lock();
                std::map< std::string, std::string >& encref = encmap.ref();
                if(encref.size() == 0) {
                    encref["&"] = "&amp;";
                    encref["<"] = "&lt;";
                    encref[">"] = "&gt;";
                }
                encmap.unlock();
                
                return recode(s,encref);
            }

            std::string decode(const std::string& s) {
                static jlib::sys::sync< std::map< std::string, std::string> > decmap;

                decmap.lock();
                std::map< std::string, std::string >& decref = decmap.ref();
                if(decref.size() == 0) {
                    decref["&amp;"] = "&";
                    decref["&lt;"] = "<";
                    decref["&gt;"] = ">";
                }
                decmap.unlock();
                
                return recode(s,decref);
            }

            std::string recode(const std::string& s, const std::map<std::string,std::string>& codec) {
                std::string ret = s;
                /*
                if(ret.length() > 1) {
                    if( (ret[0] == '"' && ret[ret.length()-1] == '"') ||
                        (ret[0] == '\'' && ret[ret.length()-1] == '\'') ) {
                        ret = ret.substr(1,ret.length()-2);
                    }
                }
                */

                std::map<std::string,std::string>::const_iterator j = codec.begin();
                std::string::size_type i;

                while(j != codec.end()) {
                    // Resume after what was just written, not from the start.
                    //
                    // This searched from position 0 every time, so encoding
                    // anything containing "&" never terminated: "&" became
                    // "&amp;", whose own "&" was then found at the same
                    // position and replaced again, and again, until the
                    // process ran out of memory.  xml::encode() was therefore
                    // unusable on any text with an ampersand in it -- which is
                    // to say, on exactly the text it exists for.
                    std::string::size_type at = 0;

                    while( (i=ret.find(j->first, at)) != std::string::npos ) {
                        ret.replace(i,j->first.length(),j->second);
                        at = i + j->second.length();
                    }

                    j++;
                }
                
                return ret;
                
            }

        }
        
        namespace file {

            struct stat getstat(const std::string& path) {
                struct stat mystat;
                if(::stat(path.c_str(), &mystat) == -1)
                    throw util_exception("error running stat(2) on "+path);
                return mystat;
            }

            long size(const std::string& path) {
                return getstat(path).st_size;
            }
            
            long mtime(const std::string& path) {
                return getstat(path).st_mtime;
            }
            
            void kill(const std::string& path, std::vector<long>& pts) {
                if(pts.size() == 0) return;
                std::sort(pts.begin(), pts.end());
                
                jlib::sys::tfstream tfs;
                std::ifstream pstream(path.c_str());
                
                std::vector<long>::iterator i = pts.begin();
                long start = 0;
                long stop = 0;
                bool bunny = true;
                while(bunny) {
                    if(i == pts.end()) {
                        stop = size(path);
                        bunny = false;
                    }
                    else {
                        stop = *i;
                    }

                    long sz = stop-start;
                    if(sz < 0) 
                        throw util_exception("trying to allocate a negative length Blob at jlib::util::file::kill");
                    //cout << "keeping from "<<start<<" to "<<stop<<", with a size of "<<sz<<std::endl;
                    std::string buffer;

                    pstream.seekg(start, std::ios_base::beg);
                    jlib::sys::getstring(pstream,buffer,sz);

                    tfs << buffer;
                    if(i != pts.end()) {
                        i++;
                        if(i != pts.end()) {
                            start = *i;
                            i++;
                        }
                        else {
                            bunny = false;
                        }
                    }
                    else {
                        bunny = false;
                    }
                }
                
                pstream.close();
                tfs.close();
                std::string cmd = "cat "+tfs.get_path()+" > "+path;
                system(cmd.c_str());
            }

            void keep(const std::string& path, std::vector<long>& pts) {
                if(pts.size() == 0) return;
                std::sort(pts.begin(), pts.end());
                
                jlib::sys::tfstream tfs;
                std::ifstream pstream(path.c_str());
                
                std::vector<long>::iterator i = pts.begin();
                while(i != pts.end()) {
                    long start = *i;
                    long stop;
                    
                    if((i+1) == pts.end()) {
                        stop = size(path);
                        i++;
                    }
                    else {
                        stop = *(i+1);
                        i+=2;
                    }
                    
                    long sz = stop-start;
                    std::string buffer;

                    pstream.seekg(start, std::ios_base::beg);
                    jlib::sys::getstring(pstream,buffer,sz);

                    tfs << buffer;
                }
                
                pstream.close();
                tfs.close();
                std::string cmd = "cat "+tfs.get_path()+" > "+path;
                system(cmd.c_str());
            }

        }
        
    }    
}

