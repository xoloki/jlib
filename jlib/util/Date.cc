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

#include <jlib/util/Date.hh>

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
            m_time = new struct tm;
            m_tz_name = create_tz_names();
            m_tz_val = create_tz_vals();
            set();
        }
        
        Date::Date(time_t secs) {
            m_time = new struct tm;
            set(secs);
        }
        
        Date::Date(struct tm* t) {
            m_time = new struct tm;
            set(t);
        }
        
        Date::~Date() {
            delete m_time;
        }
        
        void Date::set() {
            time_t now = ::time(0);
            set(localtime(&now));
            if(m_time == 0)
                throw exception("error calling localtime(time_t*) in xdbc::Date::set()");
        }
        
        /**
         * set from the seconds value passed
         */
        void Date::set(time_t secs) {
            set(localtime(&secs));
            if(m_time == 0)
                throw exception("error calling localtime(time_t*) in xdbc::Date::set(time_t)");
        }
        
        /**
         * set from the timeval* passed
         */
        void Date::set(struct tm* t) {
            m_time->tm_sec = t->tm_sec;
            m_time->tm_min = t->tm_min;
            m_time->tm_hour = t->tm_hour;
            m_time->tm_mday = t->tm_mday;
            m_time->tm_mon = t->tm_mon;
            m_time->tm_year = t->tm_year;
            m_time->tm_wday = t->tm_wday;
            m_time->tm_yday = t->tm_yday;
            m_time->tm_isdst = t->tm_isdst;
        }
        
        int Date::find_month(std::string s, name_format f) const {
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
        
        int Date::find_weekday(std::string s, name_format f) const {
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
            return mktime(m_time);
        }
        std::string Date::build_date(std::string fmt) const {
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
                os << std::setw(2) << std::setfill('0') << m_time->tm_hour ;
                break;
            case 'I':
                if(m_time->tm_hour == 0) {
                    os << std::setw(2) << std::setfill('0') << 12 ;
                }
                else if(m_time->tm_hour <= 12) {
                    os << std::setw(2) << std::setfill('0') << m_time->tm_hour ;
                }
                else {
                    os << std::setw(2) << std::setfill('0') << (m_time->tm_hour - 12) ;
                }
                break;
            case 'k':
                os << std::setw(2) << std::setfill(' ') << m_time->tm_hour ;
                break;
            case 'l':
                if(m_time->tm_hour == 0) {
                    os << std::setw(2) << std::setfill(' ') << 12 ;
                }
                else if(m_time->tm_hour <= 12) {
                    os << std::setw(2) << std::setfill(' ') << m_time->tm_hour ;
                }
                else {
                    os << std::setw(2) << std::setfill(' ') << (m_time->tm_hour - 12) ;
                }
                break;
            case 'M':
                os << std::setw(2) << std::setfill('0') << m_time->tm_min ;
                break;
            case 'p':
                if(m_time->tm_hour >= 12) {
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
                os << mktime(m_time) ;
                break;
            case 'S':
                os << std::setw(2) << std::setfill('0') << m_time->tm_sec ;
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
                os << short_weekdays[m_time->tm_wday] ;
                break;
            case 'A':
                os << long_weekdays[m_time->tm_wday] ;
                break;
            case 'b':
                os << short_months[m_time->tm_mon] ;
                break;
            case 'B':
                os << long_months[m_time->tm_mon] ;
                break;
            case 'c':
                os << build_date("%a %b %d %X %Z %Y") ;
                break;
            case 'd':
                os <<std::setw(2) <<std::setfill('0') << m_time->tm_mday;
                break;
            case 'D':
                os << build_date("%m/%d/%y") ;
                break;
            case 'h':
                os << short_months[m_time->tm_wday] ;
                break;
            case 'j':
                os <<std::setw(3) <<std::setfill('0') << m_time->tm_yday;
                break;
            case 'm':
                os <<std::setw(2) <<std::setfill('0') << (m_time->tm_mon+1);
                break;
            case 'U':
                break;
            case 'w':
                os <<std::setw(2) <<std::setfill('0') << m_time->tm_wday;
                break;
            case 'W':
                break;
            case 'x':
                os << build_date("%D") ;
                break;
            case 'y':
                os << std::setw(2) << std::setfill('0') << (m_time->tm_year % 100) ;
                break;
            case 'Y':
                os << (m_time->tm_year + 1900) ;
                break;
            default:
                throw exception("bad format std::string '"+fmt+"': the directive that failed was "+sfmt);
            }

            return (os.str()+build_date(fmt.substr(i+1)));
        }
        
        
        void Date::build_date(std::istream& is, std::string fmt) {
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
            case 'O':
                auto_parse(is);
                return;
            case 'H':
            case 'k':
                is >> ibuf;
                m_time->tm_hour = ibuf;
                break;
            case 'M':
                is >> ibuf;
                m_time->tm_min = ibuf;
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
                    m_time->tm_hour += 12;
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
                m_time->tm_sec = ibuf;
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
                  m_time->tm_hour += m_tz_val[sbuf];
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
                m_time->tm_mon = find_month(sbuf, SHORT);
                break;
            case 'B':
                is >> sbuf;
                m_time->tm_mon = find_month(sbuf, LONG);
                break;
            case 'c':
                build_date(is, "%a %b %d %X %Z %Y");
                break;
            case 'd':
                is >> ibuf;
                m_time->tm_mday = ibuf;
                break;
            case 'D':
                build_date(is, "%m/%d/%y");
                break;
            case 'h':
                is >> sbuf;
                m_time->tm_mon = find_month(sbuf, SHORT);
                break;
            case 'm':
                is >> ibuf;
                m_time->tm_mon = ibuf-1;
                break;
            case 'x':
                build_date(is, "%D");
                break;
            case 'y':
                is >> ibuf;
                if(ibuf < 70) {
                    m_time->tm_year = 100+ibuf;
                }
                else {
                    m_time->tm_year = ibuf;
                }
                
                break;
            case 'Y':
                is >> ibuf;
                m_time->tm_year  = ibuf-1900;
                break;
            default:
                throw exception("bad format std::string '"+fmt+"': the directive that failed was "+sfmt);
            }
            build_date(is, fmt.substr(i+1));
            
        }
        
        void Date::auto_parse(std::istream& is) {
            std::vector<std::string> elems;
            std::string buf, s;
            std::string fmt;
            while(!is.eof()) {
                is >> buf;
                elems.push_back(buf);
            }
            for(std::vector<std::string>::size_type i=0;i<elems.size();i++) {
                //std::cout << "i = "<<i<<", elems[i] = "<<elems[i]<<std::endl;
                elems[i] = sanitize(elems[i]);
                if(is_alpha(elems[i])) {
                    elems[i] = first_upper(elems[i]);
                    try {
                        find_month(elems[i], SHORT);
                        fmt += "%b ";
                    }
                    catch(exception& e) {
                        try {
                            find_month(elems[i], LONG);
                            fmt += "%B ";
                        }
                        catch(exception& e) {
                            try {
                                find_weekday(elems[i], SHORT);
                                fmt += "%a ";
                            }
                            catch(exception& e) {
                                try {
                                    find_weekday(elems[i], LONG);
                                    fmt += "%A ";
                                }
                                catch(exception& e) {
                                    if(is_timezone(elems[i])) {
                                        fmt += "%Z ";
                                    }
                                    else {
                                        throw exception("couldn't parse "+elems[i]+" into a valid date field");
                                    }
                                }
                            }
                            
                        }
                    }
                }
                else if(is_digit(elems[i])) {
                    if(elems[i].length() == 4) {
                        fmt += "%Y ";
                    }
                    else if(elems[i].length() == 2 || elems[i].length() == 1) {
                        int k;
                        std::istringstream isk(elems[i]);
                        isk >> k;
                        if(k > 31 || k == 0) {
                            fmt += "%y ";
                        }
                        else {
                            fmt += "%d ";
                        }
                    }
                }
                else {
                    if(is_time(elems[i])) {
                        //std::cout << elems[i] << " is_time"<<std::endl;
                        //std::cout << "i = "<<i<<std::endl;
                        if((i+1 < elems.size()) && (elems[i+1] == "AM" || elems[i+1] == "PM")) {
                            //std::cout << "elems[i+1] = " << elems[i+1] <<std::endl;
                            //std::cout << elems[i] << " is 12"<<std::endl;
                            fmt += "%r ";
                            i++;
                        }
                        else {
                            //std::cout << elems[i] << " is 24"<<std::endl;
                            fmt += "%T ";
                        }
                        //std::cout << "i = "<<i<<std::endl;
                    }
                    else if(is_timezone(elems[i])) {
                        fmt += "%Z ";
                    }
                    else{
                        throw exception("couldn't parse "+elems[i]+" into a valid date field");
                    }
                }
                
            }
            
            while(fmt[fmt.length()-1] == ' ')
                fmt.erase(fmt.length()-1);
            
            /* 
             * now that we have the format std::string built, rebuild our std::istream
             * and call build_date normally
             */
            
            for(std::vector<std::string>::size_type i=0;i<elems.size();i++)
                s += (elems[i]+" ");
            while(s[s.length()-1] == ' ')
                s.erase(s.length()-1);
            
            std::istringstream is2(s);
            //std::cout << "s = "<<s<<"; fmt = "<<fmt<<std::endl;
            build_date(is2,fmt);
        }
        
        std::string Date::get(std::string fmt) const {
            return build_date(fmt);
        }
        
        void Date::set(std::string s, std::string fmt) {
            //std::cout << "setting "<<s<<" to format "<<fmt<<std::endl;
            std::memset(m_time, 0, sizeof(struct tm));
            m_current_tz = "";
            std::istringstream is(s);
            build_date(is,fmt);
            reinit();
            //std::cout << "time() => "<<time()<<std::endl;
            //debug_print();
        }
        void Date::reinit() {
            int isdst = m_time->tm_isdst;
            time_t newtime = mktime(m_time);
            /*
              if(m_current_tz != "") {
              newtime += (3600*m_tz_val[m_current_tz]);
              }
            */
            set(localtime(&newtime));
            if(isdst == 0 && m_time->tm_isdst == 1) {
                m_time->tm_hour--;
                reinit();
            }
        }
        
        std::string Date::sanitize(std::string s) const {
            std::string ret = s;
            std::string bad = "();,\"\'[]";
            std::string::size_type i;
            while((i=ret.find_first_of(bad)) != s.npos) {
                ret.erase(i,1);
            }
            return ret;
        }
        
        bool Date::is_alpha(std::string s) const {
            for(std::string::size_type i=0; i<s.length(); i++) {
                if(!isalpha(s[i]))
                    return false;
            }
            return true;
        }
        
        bool Date::is_digit(std::string s) const {
            for(std::string::size_type i=0; i<s.length(); i++) {
                if(!isdigit(s[i]))
                    return false;
            }
            return true;
        }
        
        std::string Date::first_upper(std::string s) const {
            std::string ret = s;
            if(s.length()) {
                ret[0] = toupper(s[0]);
                for(std::string::size_type i=1; i<s.length(); i++) {
                    ret[i] = tolower(s[i]);
                }
            }
            return ret;
        }
        
        bool Date::is_time(std::string s) const {
            return (
                    s.length() == 8 &&
                    isdigit(s[0]) &&
                    isdigit(s[1]) &&
                    s[2] == ':' &&
                    isdigit(s[3]) &&
                    isdigit(s[4]) &&
                    s[5] == ':' &&
                    isdigit(s[6]) &&
                    isdigit(s[7])
                    );
        }
        
        bool Date::is_timezone(std::string s) const {
            /*
              bool nzone = ( (s.length() == 5) && (s[0] == '-' || s[0] == '+') && (is_digit(s.substr(1,4))) );
              
              bool szone = (
              s.length() == 3 || 
              s.length() == 5 || 
              (s == "/etc/localtime") 
              );
              
              std::string::size_type i = s.find("/");
              bool lszone = (i != s.npos && is_alpha(s.substr(0,i)) && is_alpha(s.substr(i+1)));
              return (nzone || szone || lszone);
            */
            // i used to actually give a damn about this field, for now
            // i'm just going to call anything I don't recognize a timezone
            return true;
        }
        
        
        bool Date::is_date(std::string s) const {
            return true;
        }
        
        std::string Date::which_date(std::string s) const {
            return s;
        }
        
        void Date::debug_print() const {
            std::cout << "m_time->tm_sec = " << m_time->tm_sec << std::endl;
            std::cout << "m_time->tm_min = " << m_time->tm_min << std::endl;
            std::cout << "m_time->tm_hour = " << m_time->tm_hour << std::endl;
            std::cout << "m_time->tm_mday = " << m_time->tm_mday << std::endl;
            std::cout << "m_time->tm_mon = " << m_time->tm_mon << std::endl;
            std::cout << "m_time->tm_year = " << m_time->tm_year << std::endl;
            std::cout << "m_time->tm_wday = " << m_time->tm_wday << std::endl;
            std::cout << "m_time->tm_yday = " << m_time->tm_yday << std::endl;
            std::cout << "m_time->tm_isdst = " << m_time->tm_isdst << std::endl;
            
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
            return m_time;
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

