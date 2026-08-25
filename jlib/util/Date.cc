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

#include <jlib/util/Date.hh>
#include <jlib/util/rfc5322.hh>

#include <jlib/util/abnf.hh>
#include <jlib/util/util.hh>

#include <sstream>
#include <iostream>
#include <iomanip>
#include <vector>

#include <cctype>
#include <cstring>
#include <cstdlib>

// in case I ever feel like adding these kinds of dates
const char* short_months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", 
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
const char* long_months[] = {"January", "February", "March", "April", "May", "June", 
                             "July", "August", "September", "October", "November", "December" };

const char* short_weekdays[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
const char* long_weekdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };

const int MONTH_MAX=12;
const int WEEK_MAX=8;

namespace jlib {
    namespace util {
        Date::Date() {
            m_tz_name = create_tz_names();
            m_tz_val = create_tz_vals();
            set();
        }
        
        Date::Date(time_t secs) {
            set(secs);
        }
        
        Date::Date(struct tm* t) {
            set(t);
        }
        
        Date::~Date() {
        }

        void Date::set() {
            time_t now = ::time(0);

            // Checked here rather than after the fact.  This tested m_time for
            // null once m_time was the thing being written, which said nothing
            // about whether localtime had failed.
            struct tm* t = localtime(&now);
            if(t == 0)
                throw exception("error calling localtime(time_t*) in xdbc::Date::set()");

            set(t);
        }
        
        /**
         * set from the seconds value passed
         */
        void Date::set(time_t secs) {
            struct tm* t = localtime(&secs);
            if(t == 0)
                throw exception("error calling localtime(time_t*) in xdbc::Date::set(time_t)");

            set(t);
        }
        
        /**
         * set from the timeval* passed
         */
        void Date::set(struct tm* t) {
            if(t == 0)
                throw exception("set(struct tm*): null");

            m_time.tm_sec = t->tm_sec;
            m_time.tm_min = t->tm_min;
            m_time.tm_hour = t->tm_hour;
            m_time.tm_mday = t->tm_mday;
            m_time.tm_mon = t->tm_mon;
            m_time.tm_year = t->tm_year;
            m_time.tm_wday = t->tm_wday;
            m_time.tm_yday = t->tm_yday;
            m_time.tm_isdst = t->tm_isdst;
        }
        
        int Date::find_month(const std::string& s, name_format f) const {
            switch(f) {
            case SHORT:
                for(int i=0; i<MONTH_MAX; i++) {
                    if(std::string(short_months[i]) == s)
                        return i;
                }
                throw exception("Bad month passed to xdbc::Date::find_month(string,xdbc::Date::name_format): passed "+s);
                break;
            case LONG:
                for(int i=0; i<MONTH_MAX; i++) {
                    if(std::string(long_months[i]) == s)
                        return i;
                }
                throw exception("Bad month passed to xdbc::Date::find_month(string,xdbc::Date::name_format): passed "+s);
                break;
            default:
                throw exception("Bad name_format passed to xdbc::Date::find_month(string,xdbc::Date::name_format)");
            }
        }
        
        int Date::find_weekday(const std::string& s, name_format f) const {
            switch(f) {
            case SHORT:
                for(int i=0; i<WEEK_MAX; i++) {
                    if(std::string(short_weekdays[i]) == s)
                        return i;
                }
                throw exception("Bad month passed to xdbc::Date::find_weekday(string,xdbc::Date::name_format): passed "+s);
                break;
            case LONG:
                for(int i=0; i<WEEK_MAX; i++) {
                    if(std::string(long_weekdays[i]) == s)
                        return i;
                }
                throw exception("Bad month passed to xdbc::Date::find_weekday(string,xdbc::Date::name_format): passed "+s);
                break;
            default:
                throw exception("Bad format passed to xdbc::Date::find_weekday(string,xdbc::Date::name_format)");
            }
        }
        time_t Date::time() const {
            // On a copy.  mktime normalizes the struct it is handed, so this
            // was writing to the object from a const method -- which compiled
            // only because m_time was a pointer, and pointing at non-const
            // data through a const member is not a const member.
            struct tm t = m_time;

            return mktime(&t);
        }
        std::string Date::build_date(const std::string& fmt) const {
            std::ostringstream os;
            std::string::size_type i = fmt.find("%");
            
            if(std::getenv("JLIB_UTIL_DATE_DEBUG"))
                std::cout << "jlib::util::build_date('" << fmt << "')" << std::endl;

            // exit condition
            if(i == fmt.npos) {
                return fmt;
            }
            
            // do we have leading text?
            if(i > 0) {
                os << fmt.substr(0,i);
            }
            i++;
            std::string sfmt;
            sfmt.append(fmt[i],1);
            switch(fmt[i]) {
            case 'H':
                os << std::setw(2) << std::setfill('0') << m_time.tm_hour ;
                break;
            case 'I':
                if(m_time.tm_hour == 0) {
                    os << std::setw(2) << std::setfill('0') << 12 ;
                }
                else if(m_time.tm_hour <= 12) {
                    os << std::setw(2) << std::setfill('0') << m_time.tm_hour ;
                }
                else {
                    os << std::setw(2) << std::setfill('0') << (m_time.tm_hour - 12) ;
                }
                break;
            case 'k':
                os << std::setw(2) << std::setfill(' ') << m_time.tm_hour ;
                break;
            case 'l':
                if(m_time.tm_hour == 0) {
                    os << std::setw(2) << std::setfill(' ') << 12 ;
                }
                else if(m_time.tm_hour <= 12) {
                    os << std::setw(2) << std::setfill(' ') << m_time.tm_hour ;
                }
                else {
                    os << std::setw(2) << std::setfill(' ') << (m_time.tm_hour - 12) ;
                }
                break;
            case 'M':
                os << std::setw(2) << std::setfill('0') << m_time.tm_min ;
                break;
            case 'p':
                if(m_time.tm_hour >= 12) {
                    os << "PM" ;
                }
                else {
                    os << "AM" ;
                }
                break;
            case 'r':
                os << build_date("%I:%M:%S %p") ;
                break;
            case 's':
                {
                    struct tm t = m_time;
                    os << mktime(&t);
                }
                break;
            case 'S':
                os << std::setw(2) << std::setfill('0') << m_time.tm_sec ;
                break;
            case 'T':
                os << build_date("%H:%M:%S");
                break;
            case 'X':
                os << build_date("%H:%M:%S");
                break;
            case 'z':
                os << static_cast< std::map<std::string,std::string> >(m_tz_name)[tzname[0]] ;
                break;
            case 'Z':
                os << tzname[0] ;
                break;
            case 'a':
                os << short_weekdays[m_time.tm_wday] ;
                break;
            case 'A':
                os << long_weekdays[m_time.tm_wday] ;
                break;
            case 'b':
                os << short_months[m_time.tm_mon] ;
                break;
            case 'B':
                os << long_months[m_time.tm_mon] ;
                break;
            case 'c':
                os << build_date("%a %b %d %X %Z %Y") ;
                break;
            case 'd':
                os <<std::setw(2) <<std::setfill('0') << m_time.tm_mday;
                break;
            case 'D':
                os << build_date("%m/%d/%y") ;
                break;
            case 'h':
                os << short_months[m_time.tm_wday] ;
                break;
            case 'j':
                os <<std::setw(3) <<std::setfill('0') << m_time.tm_yday;
                break;
            case 'm':
                os <<std::setw(2) <<std::setfill('0') << (m_time.tm_mon+1);
                break;
            case 'U':
                break;
            case 'w':
                os <<std::setw(2) <<std::setfill('0') << m_time.tm_wday;
                break;
            case 'W':
                break;
            case 'x':
                os << build_date("%D") ;
                break;
            case 'y':
                os << std::setw(2) << std::setfill('0') << (m_time.tm_year % 100) ;
                break;
            case 'Y':
                os << (m_time.tm_year + 1900) ;
                break;
            default:
                throw exception("bad format std::string '"+fmt+"': the directive that failed was "+sfmt);
            }

            return (os.str()+build_date(fmt.substr(i+1)));
        }
        
        
        void Date::build_date(std::istream& is, const std::string& fmt) {
            std::string::size_type i = fmt.find("%");
            
            int ibuf;
            std::string sbuf;
            time_t tbuf;
            
            // exit condition
            if(i == fmt.npos || !is) {
                return;
            }
            
            // do we have leading text?
            if(i > 0) {
                is.seekg(i, std::ios_base::cur);
            }
            i++;
            
            std::string sfmt;
            sfmt.append(fmt[i],1);
            switch(fmt[i]) {
            case 'O': {
                // "%O" is an RFC 5322 date-time, and set() intercepts it
                // before build_date is ever called with one.  Reaching here
                // means the directive is embedded in a longer format string,
                // which it cannot be: a date-time is the whole of the value.
                std::string rest;

                std::getline(is, rest);

                set(rest, "%O");

                return;
            }
            case 'H':
            case 'k':
                is >> ibuf;
                m_time.tm_hour = ibuf;
                break;
            case 'M':
                is >> ibuf;
                m_time.tm_min = ibuf;
                break;
            case 'p':
                is >> sbuf;
                break;
            case 'r':
                //std::cout << "we're at case 'r':"<<std::endl;
                debug_print();
                build_date(is, "%H:%M:%S");
                debug_print();
                is >> sbuf;
                //std::cout << "sbuf = "<<sbuf <<std::endl;
                if(sbuf == "AM") {
                }
                else if(sbuf == "PM") {
                    m_time.tm_hour += 12;
                }
                else {
                    throw exception("bad AM/PM specifier '"+sbuf+"' in format '"+fmt+"'");
                }
                debug_print();
            case 's':
                is >> tbuf;
                set(localtime(&tbuf));
                break;
            case 'S':
                is >> ibuf;
                m_time.tm_sec = ibuf;
                break;
            case 'T':
                build_date(is, "%H:%M:%S");
                break;
            case 'X':
                build_date(is, "%H:%M:%S");
                break;
            case 'Z':
                is >> sbuf;
                /*
                  if(m_tz_name[sbuf] != "") {
                  m_time.tm_hour += m_tz_val[sbuf];
                  }
                */
                break;
            case 'a':
                is >> sbuf;
                break;
            case 'A':
                is >> sbuf;
                break;
            case 'b':
                is >> sbuf;
                m_time.tm_mon = find_month(sbuf, SHORT);
                break;
            case 'B':
                is >> sbuf;
                m_time.tm_mon = find_month(sbuf, LONG);
                break;
            case 'c':
                build_date(is, "%a %b %d %X %Z %Y");
                break;
            case 'd':
                is >> ibuf;
                m_time.tm_mday = ibuf;
                break;
            case 'D':
                build_date(is, "%m/%d/%y");
                break;
            case 'h':
                is >> sbuf;
                m_time.tm_mon = find_month(sbuf, SHORT);
                break;
            case 'm':
                is >> ibuf;
                m_time.tm_mon = ibuf-1;
                break;
            case 'x':
                build_date(is, "%D");
                break;
            case 'y':
                is >> ibuf;
                if(ibuf < 70) {
                    m_time.tm_year = 100+ibuf;
                }
                else {
                    m_time.tm_year = ibuf;
                }
                
                break;
            case 'Y':
                is >> ibuf;
                m_time.tm_year  = ibuf-1900;
                break;
            default:
                throw exception("bad format std::string '"+fmt+"': the directive that failed was "+sfmt);
            }
            build_date(is, fmt.substr(i+1));
            
        }
        
        std::string Date::get(const std::string& fmt) const {
            return build_date(fmt);
        }
        
        namespace {

            /** Built on first use, for the reason at crypt/curve.hh:42. */
            const abnf::grammar& dates() {
                static abnf::grammar g = [] {
                    abnf::grammar g = abnf::compile(std::string(rfc5322::LEXICAL) +
                                                    rfc5322::DATE_TIME);
                    g.check();

                    return g;
                }();

                return g;
            }

            abnf::options parse_options() {
                abnf::options o;

                o.captures = abnf::options::capture_policy::listed;
                o.capture_only = { "day-digits", "month", "year-digits",
                                   "hour-digits", "minute-digits",
                                   "second-digits",
                                   "zone-sign", "zone-offset", "obs-zone" };

                return o;
            }

            int month_of(std::string_view name) {
                static const char* const MONTHS[] = {
                    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
                };

                for(int i = 0; i < 12; i++) {
                    if(name == MONTHS[i]) return i;
                }

                return 0;
            }

            /**
             * RFC 5322 4.3, the zones that are names.
             *
             * The single military letters are deliberately absent: 4.3 says
             * they "were defined incorrectly" in RFC 822 and that a parser
             * SHOULD treat them as -0000, which is what falling through to
             * zero does.  Reading "A" as +0100 would be following the older
             * document's mistake.
             */
            int offset_of(const std::string& name) {
                static const std::map<std::string, int> ZONES = {
                    { "UTC",   0 }, { "UT",    0 }, { "GMT",   0 },
                    { "EST", -300 }, { "EDT", -240 },
                    { "CST", -360 }, { "CDT", -300 },
                    { "MST", -420 }, { "MDT", -360 },
                    { "PST", -480 }, { "PDT", -420 },
                };

                const std::map<std::string, int>::const_iterator i =
                    ZONES.find(upper(name));

                return i == ZONES.end() ? 0 : i->second;
            }

            /**
             * Days from 1970-01-01 to a civil date, Howard Hinnant's algorithm.
             *
             * Rather than timegm(), which is not in any C or C++ standard, or
             * mktime(), which would apply the local zone to a time that
             * already carries its own.
             */
            long long days_from_civil(long long y, unsigned m, unsigned d) {
                y -= m <= 2;

                const long long era = (y >= 0 ? y : y - 399) / 400;
                const unsigned yoe = static_cast<unsigned>(y - era * 400);
                const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
                const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

                return era * 146097 + static_cast<long long>(doe) - 719468;
            }

        }

        time_t Date::parse_rfc5322(const std::string& s) {
            abnf::match m;

            try {
                m = dates().at("date-time").parse(trim(s), parse_options());
            }
            catch(abnf::budget_exceeded& e) {
                throw exception(std::string("jlib::util::Date: gave up reading \"")
                                + s + "\": " + e.what());
            }
            catch(abnf::error& e) {
                throw exception("jlib::util::Date: \"" + s + "\" is not an RFC 5322 "
                                "date-time, at column " + std::to_string(e.column()));
            }

            const int day = int_value(m["day-digits"].str());
            const int mon = month_of(m["month"].text());

            int year = int_value(m["year-digits"].str());
            const std::size_t digits = m["year-digits"].str().size();

            // RFC 5322 4.3: a two-digit year 00-49 is 20xx and 50-99 is 19xx;
            // three digits are the year minus 1900.  This is the rule the
            // obsolete syntax needs and the old code had no notion of -- it
            // decided between %y and %Y by whether the token was four
            // characters long and left the rest to strptime.
            if(digits == 2)      year += (year < 50 ? 2000 : 1900);
            else if(digits == 3) year += 1900;

            const int hour = int_value(m["hour-digits"].str());
            const int min = int_value(m["minute-digits"].str());
            const int sec = m["second-digits"] ? int_value(m["second-digits"].str()) : 0;

            int offset = 0;

            if(m["zone-offset"]) {
                const std::string o = m["zone-offset"].str();

                offset = int_value(o.substr(0, 2)) * 60 + int_value(o.substr(2, 2));

                if(m["zone-sign"].text() == "-") offset = -offset;
            }
            else if(m["obs-zone"]) {
                offset = offset_of(m["obs-zone"].str());
            }

            const long long days = days_from_civil(year, static_cast<unsigned>(mon + 1),
                                                   static_cast<unsigned>(day));

            return static_cast<time_t>(days * 86400 + hour * 3600 + min * 60 + sec
                                       - offset * 60);
        }

        bool Date::valid(const std::string& s) {
            abnf::options o;

            o.captures = abnf::options::capture_policy::none;

            return static_cast<bool>(dates().at("date-time").try_parse(trim(s), o));
        }

        void Date::set(const std::string& s, const std::string& fmt) {
            std::memset(&m_time, 0, sizeof(struct tm));
            m_current_tz = "";

            if(fmt == "%O") {
                set(parse_rfc5322(s));
                return;
            }

            std::istringstream is(s);
            build_date(is,fmt);
            reinit();
            //std::cout << "time() => "<<time()<<std::endl;
            //debug_print();
        }
        void Date::reinit() {
            int isdst = m_time.tm_isdst;
            time_t newtime = mktime(&m_time);
            /*
              if(m_current_tz != "") {
              newtime += (3600*m_tz_val[m_current_tz]);
              }
            */
            set(localtime(&newtime));
            if(isdst == 0 && m_time.tm_isdst == 1) {
                m_time.tm_hour--;
                reinit();
            }
        }
        
        
        
        
        std::string Date::first_upper(const std::string& s) const {
            std::string ret = s;
            if(s.length()) {
                ret[0] = toupper(s[0]);
                for(std::string::size_type i=1; i<s.length(); i++) {
                    ret[i] = tolower(s[i]);
                }
            }
            return ret;
        }
        
        
        
        
        
        
        void Date::debug_print() const {
            std::cout << "m_time.tm_sec = " << m_time.tm_sec << std::endl;
            std::cout << "m_time.tm_min = " << m_time.tm_min << std::endl;
            std::cout << "m_time.tm_hour = " << m_time.tm_hour << std::endl;
            std::cout << "m_time.tm_mday = " << m_time.tm_mday << std::endl;
            std::cout << "m_time.tm_mon = " << m_time.tm_mon << std::endl;
            std::cout << "m_time.tm_year = " << m_time.tm_year << std::endl;
            std::cout << "m_time.tm_wday = " << m_time.tm_wday << std::endl;
            std::cout << "m_time.tm_yday = " << m_time.tm_yday << std::endl;
            std::cout << "m_time.tm_isdst = " << m_time.tm_isdst << std::endl;
            
        }
        
        std::map<std::string,std::string> Date::create_tz_names() {
            std::map<std::string,std::string> ret;
            ret["GMT"] = "+0000";
            ret["EDT"] = "-0400";
            ret["EST"] = "-0500";
            ret["CDT"] = "-0500";
            ret["CST"] = "-0600";
            ret["MDT"] = "-0600";
            ret["MST"] = "-0700";
            ret["PDT"] = "-0700";
            ret["PST"] = "-0800";
            
            return ret;
        }
        
        std::map<std::string,int> Date::create_tz_vals() {
            std::map<std::string,int> ret;
            ret["GMT"] = 0;
            ret["EDT"] = -4;
            ret["EST"] = -5;
            ret["CDT"] = -5;
            ret["CST"] = -6;
            ret["MDT"] = -6;
            ret["MST"] = -7;
            ret["PDT"] = -7;
            ret["PST"] = -8;
            
            return ret;
        }
        
        struct tm* Date::stm() {
            return &m_time;
        }
        
        
        std::istream& operator>>(std::istream& in, jlib::util::Date& d) {
            std::memset(d.stm(), 0, sizeof(struct tm));
            d.build_date(in,"%O");
            d.reinit();
            return in;
        }
        std::ostream& operator<<(std::ostream& out, const jlib::util::Date& d) {
            out << d.get();
            return out;
        }
        
    }
}

