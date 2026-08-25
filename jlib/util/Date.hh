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

#ifndef JLIB_UTIL_DATE_HH
#define JLIB_UTIL_DATE_HH

#include <ctime>

#include <string>
#include <exception>
#include <map>

// #ifdef XDBC_COMPILE

// #else

// #endif

namespace jlib {
    namespace util {
        /**
         * class Date is an attempt to provide a user friendly wrapper for the
         * various C time/date functions (time, localtime, mktime, etc).  You 
         * can create Date objects using either time_t or struct timeval* values;
         * you can get/set the time in either predefined or custom format.
         *
         * For the custom formatting, you provide a std::string containing directives,
         * whitespace, and text.  The whitespace and text comes back unchanged,
         * while the directives are translated as per a subset of the date(1) UNIX 
         * command.  The supported directive are listed below:
         *
         * Time fields:
         * 
         * %H     hour (00..23)
         * 
         * %I     hour (01..12)
         * 
         * %k     hour ( 0..23)
         * 
         * %l     hour ( 1..12)
         * 
         * %M     minute (00..59)
         * 
         * %p     locale's AM or PM
         * 
         * %r     time, 12-hour (hh:mm:ss [AP]M)
         * 
         * %s     seconds since 1970-01-01 00:00:00 UTC  (a  
         *        nonstandard extension)
         * 
         * %S     second (00..61)
         * 
         * %T     time, 24-hour (hh:mm:ss)
         * 
         * %X     locale's time representation (%H:%M:%S)
         * 
         * %Z     time  zone  (e.g., EDT), or nothing if no time zone
         *        is determinable
         * 
         * Date fields:
         * 
         * %a     locale's abbreviated weekday name (Sun..Sat)
         * 
         * %A     locale's full weekday name, variable  length  
         *        (Sun-day..Saturday)
         * 
         * %b     locale's abbreviated month name (Jan..Dec)
         * 
         * %B     locale's  full  month  name,  variable length
         *        (January..December)
         * 
         * %c     locale's date and time (Sat  Nov  04  12:02:33  EST
         *        1989)
         * 
         * %d     day of month (01..31)
         * 
         * %D     date (mm/dd/yy)
         * 
         * %h     same as %b
         * 
         * %j     day of year (001..366)
         * 
         * %m     month (01..12)
         * 
         * %U     week  number  of  year  with Sunday as first day of
         *        week (00..53)
         * 
         * %w     day of week (0..6) with 0 corresponding to Sunday
         * 
         * %W     week number of year with Monday  as  first  day  of
         *        week (00..53)
         * 
         * %x     locale's date representation (mm/dd/yy)
         * 
         * %y     last two digits of year (00..99)
         * 
         * %Y     year (1970...)
         *
         */
        class Date {
        public:
            typedef enum { SHORT, LONG } name_format;

            class exception : public std::exception {
            public:
                exception(const std::string& msg = "date exception") : m_msg(msg) {}
                virtual ~exception() {}
                virtual const char* what() const noexcept { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };
            
        public:
            Date();
            Date(time_t secs);
            Date(struct tm* t);
            virtual ~Date();

            /**
             * The rest of the five, explicitly.
             *
             * A user-declared destructor -- which a polymorphic class needs,
             * and which also anchors the vtable -- suppresses the implicit
             * move constructor and move assignment, so every std::move of a
             * Date was quietly doing a copy.  Defaulting them says what was
             * meant and costs nothing now that m_time is a value.
             */
            Date(const Date&) = default;
            Date& operator=(const Date&) = default;
            Date(Date&&) = default;
            Date& operator=(Date&&) = default;
            
            /**
             * set from the current time
             */
            virtual void set();
            
            /**
             * set from the seconds value passed
             */
            virtual void set(time_t secs);
            
            /**
             * set from the timeval* passed
             */
            virtual void set(struct tm* t);
            
            virtual std::string get(const std::string& fmt="%a, %d %b %Y %H:%M:%S %z") const;

            /**
             * Read a date.
             *
             * The default format "%O" means an RFC 5322 date-time -- the thing
             * in a Date: header -- read against section 3.3's grammar, in
             * jlib/util/rfc5322.hh.  Section 4.3's obsolete forms are accepted,
             * because a Date: header is written by whoever sent the message
             * and refusing one means refusing to sort a mailbox.
             *
             * Any other format string is a strftime-style one and is read by
             * build_date() as before.
             *
             * Throws Date::exception on a date it cannot read.  It used to
             * guess: is_timezone() returned true for every input, so the two
             * "couldn't parse" throws below it were unreachable and any
             * garbage became a format string that strptime then half-applied.
             */
            virtual void set(const std::string& s, const std::string& fmt="%O");

            /** Would it parse as an RFC 5322 date-time? */
            static bool valid(const std::string& s);

            /**
             * An RFC 5322 date-time as seconds since the epoch.
             *
             * The zone in the header is applied, so this is an absolute
             * instant and does not depend on where the machine reading it is.
             * Throws Date::exception.
             */
            static time_t parse_rfc5322(const std::string& s);
            
            /**
             * return the current date as a time_t
             */
            time_t time() const;
            
            int year() const { return m_time.tm_year; }
            int mon() const { return m_time.tm_mon; }
            int mday() const { return m_time.tm_mday; }
            int hour() const { return m_time.tm_hour; }
            int min() const { return m_time.tm_min; }
            int sec() const { return m_time.tm_sec; }
            
            friend std::istream& operator>>(std::istream& in, Date& d);
            friend std::ostream& operator<<(std::ostream& out, const Date& d);
            
            /**
             * given a month as string, return it's integer rep
             */
            int find_month(const std::string& s, name_format f) const;
            int find_weekday(const std::string& s, name_format f) const;
            
            /**
             * recursively build date std::string from current tm and passed
             * fmt string
             */
            std::string build_date(const std::string& fmt) const;
            
            /**
             * recursively build tm from passed date s and format fmt
             *
             * only specific time and date directives are allowed: 
             * the clustered directives are ignored, as are the day of week
             * and day of year
             */
            void build_date(std::istream& is, const std::string& fmt);
            

            /**
             * reinitialize m_time by calling mktime() and localtime(), 
             * successively
             */
            void reinit();
            


            /**
             * convert the std::string to first letter uppercase, ow lower
             *
             */
            std::string first_upper(const std::string& s) const;
            
            /**
             * print the contents of each member of the struct tm*
             *
             */
            void debug_print() const;
            
            /**
             * create the timezone name => num mapping, i.e.
             * "EDT" => "-0400"
             */
            static std::map<std::string,std::string> create_tz_names();
            
            /**
             * create the timezone name => val mapping, i.e.
             * "EDT" => -4
             */
            static std::map<std::string,int> create_tz_vals();
            
            struct tm* stm();
            
        private:
            /**
             * The broken-down time, by value.
             *
             * This was a struct tm* allocated with new in every constructor
             * and deleted in the destructor -- for a POD of nine ints, which
             * needs no allocation at all.  Worse, no copy constructor was
             * declared, so copying a Date copied the pointer and both
             * destructors freed it: ASan reported the double free at
             * Date.cc:63.
             *
             * As a value the copy is correct and free, and with the
             * destructor gone the implicit move comes back.
             */
            struct tm m_time;
            std::map<std::string,std::string> m_tz_name;
            std::map<std::string,int> m_tz_val;
            
            std::string m_current_tz;
        };
        
    }
}

#endif
