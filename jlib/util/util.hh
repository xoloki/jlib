/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joe Yandle <jwy@divisionbyzero.com>
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

#ifndef JLIB_UTIL_HH
#define JLIB_UTIL_HH

#include <sys/stat.h>

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
            util_exception(std::string p_msg = "") {
                m_msg = "util exception: "+p_msg;
            }
            virtual ~util_exception() throw() {}
            virtual const char* what() const throw() { return m_msg.c_str(); }
        protected:
            std::string m_msg;
        };
      
        /**
         * Convert to upper case.
         *
         * @param s std::string to convert
         * @return s with all characters in upper case
         */
        std::string upper(std::string s);

        /**
         * make the first character and every char following a '-' caps
         */
        std::string studly_caps(std::string s);
        
        /**
         * Convert to lower case.
         *
         * @param s std::string to convert
         * @return s with all characters in upper case
         */
        std::string lower(std::string s);
        
        /**
         * Tokenize this std::string with the given delimiter.
         *
         * @param s std::string to tokenize
         * @param d delimiter for tokenization
         * @return vector of strings
         */

        std::vector<std::string> tokenize(std::string s, std::string d = " ", bool split_delim = true);

        std::list<std::string> tokenize_list(std::string s, std::string d = "/", bool split_delim = false);
        
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
        int intValue(std::string s, int base = 10);
        int int_value(std::string s, int base = 10);
        
        /**
         * Get a double from a string.
         *
         * @param s std::string to convert to double
         * @return s converted to double
         */
        double doubleValue(std::string s);
        double double_value(std::string s);

        std::string hex_value(unsigned char c, bool upper=false);
        std::string hex_value(std::string s, bool upper=false);
        std::string hex_value(const unsigned char* data, std::size_t size, bool upper=false);
        
        /**
         * Remove whitespace from beginning of passed string.
         *
         * @param s std::string to chip
         * @return s without leading whitespace
         */
        std::string chip(std::string s);
        
        /**
         * Remove whitespace from end of passed string.
         *
         * @param s std::string to chop
         * @return s without trailing whitespace
         */
        std::string chop(std::string s);
        
        /**
         * Remove whitespace from beginning and end of passed string.
         *
         * @param s std::string to trim
         * @return s without leading or trailing whitespace
         */
        std::string trim(std::string s);
        
        /**
         * Remove characters between passed delimiters
         *
         * @param s std::string to excise
         * @param d1 beginning delimiter
         * @param d2 ending delimiter
         * @return s without d1, d2, or anything between
         */
        std::string excise(std::string s, std::string d1, std::string d2);
        
        /**
         * Remove characters except between passed delimiters
         *
         * @param s std::string to excise
         * @param d1 beginning delimiter
         * @param d2 ending delimiter
         * @return s between d1 and d2, or unchanged if s doesn't contain d1 and d2
         */
        std::string slice(std::string s, std::string d1, std::string d2);
        
        /**
         * Tell if t is a substd::string of s
         *
         * @param s haystack
         * @param t needle
         * @return true if s contains t, ow false
         */
        bool contains(std::string s, std::string t);
        
        /**
         * Tell if s begins with t.
         *
         * @param s haystack
         * @param t needle
         * @return true if s begins with t, ow false
         */
        bool begins(std::string s, std::string t);
        
        /**
         * Tell if s ends with t.
         *
         * @param s haystack
         * @param t needle
         * @return true if s ends with t, ow false
         */
        bool ends(std::string s, std::string t);
        
        /**
         * Tell if t is a substd::string of s
         *
         * @param s haystack
         * @param t needle
         * @return true if s contains t, ow false
         */
        bool icontains(std::string s, std::string t);
        
        /**
         * Tell if s begins with t.
         *
         * @param s haystack
         * @param t needle
         * @return true if s begins with t, ow false
         */
        bool ibegins(std::string s, std::string t);
        
        /**
         * Tell if s ends with t.
         *
         * @param s haystack
         * @param t needle
         * @return true if s ends with t, ow false
         */
        bool iends(std::string s, std::string t);
        
        /**
         * Tell if s equals t, case insensitive.
         *
         * @param s str1
         * @param t str2
         * @return true if s equals t, ow false
         */
        bool iequals(std::string s, std::string t);

        bool imaps(const std::map<std::string,std::string>& m, std::string key, std::string val);
        
        template<class T>
        T get(std::string s, unsigned int offset=0) {
            return *reinterpret_cast<T*>(const_cast<char*>(s.data())+offset);
        }
        
        template<class T>
        T get(const char* c, unsigned int offset=0) {
            return *reinterpret_cast<T*>(const_cast<char*>(c)+offset);
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
         * namespace base64 contains functions that encode and decode base64 encryption
         */
        namespace base64 {
            
            /**
             * Decode the std::string into a blob.
             *
             * @param s std::string to parse data from
             * @return decoded data
             */
            std::string decode(std::string s);
            
            /**
             * Encode the blob into a string.
             *
             * @param s std::string to parse data from
             * @return encoded data
             */
            std::string encode(std::string s);

        }
        
        namespace qp {
            
            /**
             * Decode the std::string into a blob.
             *
             * @param s std::string to parse data from
             * @return decoded data
             */
            std::string decode(std::string s);
            
            /**
             * Encode the blob into a string.
             *
             * @param s std::string to parse data from
             * @return encoded data
             */
            std::string encode(std::string s);

        }


        namespace uri {
            std::string encode(std::string s);
            std::string decode(std::string s);
        }
       
        
        namespace xml {
            std::string encode(std::string s);
            std::string decode(std::string s);
            std::string recode(std::string s, const std::map<std::string,std::string>& codec);
        }
       
        namespace file {

            struct stat getstat(std::string path);
            long size(std::string path);
            long mtime(std::string path);
            
            /**
             * slice out from the file at path the regions marked in pts
             * the even indicies are start points, the odds are end points
             * if we have an uneven number, slice out from the last point 
             * to the end
             *
             * the idea here is to kget rid of these regions, and keep the rest
             */
            void kill(std::string path, std::vector<long>& pts);

            /**
             * slice out from the file at path the regions marked in pts
             * the even indicies are start points, the odds are end points
             * if we have an uneven number, slice out from the last point 
             * to the end
             *
             * the idea here is to keep these regions, and get rid of the rest
             */
            void keep(std::string path, std::vector<long>& pts);

        }
    }
}
#endif
