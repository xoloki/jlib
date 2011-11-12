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

#include <jlib/sys/tfstream.hh>
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

        std::string studly_caps(std::string s) {
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


        std::vector<std::string> tokenize(std::string s, std::string d, bool split_delim) {
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

        std::list<std::string> tokenize_list(std::string s, std::string d, bool split_delim) {
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

        std::string excise(std::string s, std::string d1, std::string d2) {
            std::string ret = s;
            int i,j=0;
            
            while( (i=ret.find(d1,j)) != -1 && (j=ret.find(d2,i+1)) != -1 ) {
                ret.erase(i,j+1-i);
                j = i;
                //cout << "Excising '" << ret.c_str() << "' between '" << d1.c_str() << "' and '" << d2.c_str() << "' = '" << ret.c_str() << "'\n";
            }
            
            return ret;
        }
        
        std::string slice(std::string s, std::string d1, std::string d2) {
            std::string::size_type i, j;
            
            if( (i=s.find(d1)) != s.npos && (j=s.find(d2,i+1)) != s.npos ) {
                return s.substr(i+1,j-i-1);
            }
            else {
                return s;
            }
        }
        
        std::string chip(std::string s) {
            std::string::size_type i = s.find_first_not_of(WHITESPACE);
            if(i != s.npos) {
                return s.substr(i);
            }
            else {
                return "";
            }
        }
        
        std::string chop(std::string s) {
            std::string::size_type i = s.find_last_not_of(WHITESPACE);
            if(i != s.npos) {
                return s.substr(0,i+1);
            }
            else {
                return "";
            }
        }
        
        std::string trim(std::string s) {
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

        bool imaps(const std::map<std::string,std::string>& m, std::string key, std::string val) {
            return (upper(const_cast< std::map<std::string,std::string>& >(m)[key]) == upper(val));
        }
        
        std::string upper(std::string s) {
            std::string ret = s;
            for(std::string::size_type i=0; i<ret.size(); i++) {
                ret[i] = toupper(ret[i]);
            }
            return ret;
        }
        
        std::string lower(std::string s) {
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
        
        int int_value(std::string s, int base) { return intValue(s,base); }
        int intValue(std::string s, int base) {
            return strtol(s.c_str(), NULL, base);
        }
        
        double double_value(std::string s) { return double_value(s); }
        double doubleValue(std::string s) {
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

        std::string hex_value(std::string s, bool upper) {
            std::string ret;
            for(std::string::size_type i=0;i<s.length();i++) {
                ret += hex_value(static_cast<unsigned char>(s[i]));
                if(i+1 < s.length())
                    ret += " ";
            }
            return ret;
        }

        bool contains(std::string s, std::string t) {
            return (s.size() >= t.size() && (int)s.find(t) != -1);
        }
        
        bool begins(std::string s, std::string t) {
            return (s.size() >= t.size() && s.find(t) == 0);
        }
        
        bool ends(std::string s, std::string t) {
            return ( s.size() >= t.size() && s.rfind(t) == (s.size()-t.size()) );
        }
        
        bool icontains(std::string s, std::string t) {
            return (s.size() >= t.size() && (int)upper(s).find(upper(t)) != -1);
        }
        
        bool ibegins(std::string s, std::string t) {
            return (s.size() >= t.size() && upper(s).find(upper(t)) == 0);
        }
        
        bool iends(std::string s, std::string t) {
            return ( s.size() >= t.size() && upper(s).rfind(upper(t)) == (s.size()-t.size()) );
        }
        
        bool iequals(std::string s, std::string t) {
            return ( upper(s) == upper(t) );
        }        

        namespace file {

            struct stat getstat(std::string path) {
                struct stat mystat;
                if(::stat(path.c_str(), &mystat) == -1)
                    throw util_exception("error running stat(2) on "+path);
                return mystat;
            }

            long size(std::string path) {
                return getstat(path).st_size;
            }
            
            long mtime(std::string path) {
                return getstat(path).st_mtime;
            }
            
            void kill(std::string path, std::vector<long>& pts) {
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

            void keep(std::string path, std::vector<long>& pts) {
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

