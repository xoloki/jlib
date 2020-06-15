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
                return s.substr(i+d1.size(),j-(i+d1.size()));
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
        
        double double_value(std::string s) { return doubleValue(s); }
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

        std::string hex_value(const unsigned char* data, std::size_t size, bool upper) {
            std::string ret;
            for(std::size_t i = 0; i < size; i++) {
                ret += hex_value(data[i]);
                if(i + 1 < size)
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

        namespace base64 {
            
            static char decode_table[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 0, 0, 0, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 0, 0, 0, 0, 0, 0, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51  };
            
            int decode(const char* in, char* out) {
                if(in[2] == '=') {
                    out[0] = ((decode_table[in[0]] << 2 ) | (0x03)) & ((0xfc) | (decode_table[in[1]] >> 4));
                    return 1;
                }
                else if(in[3] == '=') {
                    out[0] = ((decode_table[in[0]] << 2 ) | (0x03)) & ((0xfc) | (decode_table[in[1]] >> 4));
                    out[1] = ((decode_table[in[1]] << 4 ) | (0x0f)) & ((0xf0) | (decode_table[in[2]] >> 2));
                    return 2;
                }
                else {
                    out[0] = ((decode_table[in[0]] << 2 ) | (0x03)) & ((0xfc) | (decode_table[in[1]] >> 4));
                    out[1] = ((decode_table[in[1]] << 4 ) | (0x0f)) & ((0xf0) | (decode_table[in[2]] >> 2));
                    out[2] = ((decode_table[in[2]] << 6 ) | (0x3f)) & ((0xc0) | (decode_table[in[3]] >> 0));
                    return 3;
                }
            }
            

            std::string decode(std::string s) {
                char* out = new char[s.length()];
                const char* in = s.data();
                
                int j=0;
                for(unsigned int i=0; i<s.length(); i+=4) {
                    while(i<s.length() && s[i] < 0x20) {
                        i++;
                    }
                    if(i<s.length()) {
                        j += decode(in+i, out+j);
                    }
                }

                std::string ret(out,j);
                delete [] out;
                return ret;
            }
          

            static unsigned char encode_table[] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/' };

            int encode(const char* in, char* out, int sz) {
                if(sz < 1) {
                    return 0;
                }
                else if(sz == 1) {
                    out[0] = encode_table[(in[0] >> 2) & (0x3f)];
                    out[1] = encode_table[(((in[0] << 4) & (0x3f)) | 0x0f) & ((0xf0))];
                    return 2;
                }
                else if(sz == 2) {
                    out[0] = encode_table[(in[0] >> 2) & (0x3f)];
                    out[1] = encode_table[(((in[0] << 4) & (0x3f)) | 0x0f) & ((0xf0) | (in[1] >> 4))];
                    out[2] = encode_table[(((in[1] << 2) & (0x3f)) | 0x03) & ((0xfc))];
                    return 3;
                }
                else if(sz == 3) {
                    out[0] = encode_table[(in[0] >> 2) & (0x3f)];
                    out[1] = encode_table[(((in[0] << 4) & (0x3f)) | 0x0f) & ((0xf0) | (in[1] >> 4))];
                    out[2] = encode_table[(((in[1] << 2) & (0x3f)) | 0x03) & ((0xfc) | (in[2] >> 6))];
                    out[3] = encode_table[(in[2]) & (0x3f)];
                    return 4;
                }
                else {
                    return 0;
                }
            }
            
            std::string encode(std::string s) {
                char* outbuf = new char[2*s.length()];
                int sz = 0;
                u_int j = 0;
                const char* inbuf = s.data();
                
                bool first = true;
                
                while(j < s.length()) {
                    u_int num = 3;
                    if((s.length() - j) < num) {
                        num = s.length() - j;
                    }
                    sz += encode(inbuf+j, outbuf+sz, num);
                    j += 3;
                    if( (first && (sz % 64) == 0) || (!first && ((sz+1) % 65) == 0) ) {
                        outbuf[sz++] = '\n';
                    }
                    if(sz >= 64) {
                        first = false;
                    }
                }
                int numeq = 0;
                switch(s.length() % 3) {
                case 1:
                    numeq = 2;
                    break;
                case 2:
                    numeq = 1;
                    break;
                default:
                    break;
                    
                }
                for(int i = 0; i<numeq; i++) {
                    outbuf[sz++] = '=';
                }
            
                //outbuf[sz] = 0;
                std::string ret(outbuf,sz);
                delete [] outbuf;
                return ret;                
            }
            
        }
        
        namespace qp {
            
            std::string decode(std::string s) {
                std::string ret;
                
                int i=0;
                int j=i;
                
                while((unsigned int)i<s.length() && (j=s.find("=", i)) != -1) {
                    ret += s.substr(i,j-i);
                    if(s[j+1] == '\n') {
                        i = j+2;
                    }
                    else if(s[j+1] == '\r' && s[j+2] == '\n') {
                        i = j+3;
                    }
                    else {
                        std::string ch = s.substr(j+1,2);
                        char val = std::strtol(ch.c_str(), NULL, 16);
                        ret.append(1,val);
                        i = j+3;
                    }
                }
                ret += s.substr(i);
                return ret;
            }

            std::string encode(std::string s) {
                std::string ret;
                
                //TODO: write qp::encode()
                
                return ret;
            }

        }


        namespace uri {

            const std::string reserved = ";/?:@&=+$,%";

	    std::string hex_value(unsigned char c, bool upper) {
		const unsigned int size=16;
		char buffer[size];

		snprintf(buffer, size-1, "%02x", c);

		std::string ret(buffer);

		if(upper) {
		    for (unsigned int i = 0; i < ret.size(); i++) {
			ret[i] = toupper(ret[i]);
		    }
		    return ret;
		}
		else {
		    return ret;
		}

	    }


            std::string encode(std::string s) {
                std::string::size_type i, j=0;
                std::string ret = s;
                while( (i=ret.find_first_of(reserved,j)) != std::string::npos ) {
                    std::string tmp = "%" + hex_value(ret[i], false);
                    ret.replace(i,1,tmp);
                    j = i+tmp.length();
                }
                return ret;
            }

            std::string decode(std::string s) {
                std::string ret = s;
                std::string::size_type i;
                while( (i=ret.find("%")) != ret.npos ) {
                    std::string hex = ret.substr(i+1,2);
                    char c = strtol(hex.c_str(), NULL, 16);
                    ret.replace(i,3,1,c);
                }

                return ret;
            }

        }

        namespace xml {

            std::string encode(std::string s) {
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

            std::string decode(std::string s) {
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

            std::string recode(std::string s, const std::map<std::string,std::string>& codec) {
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
                    while( (i=ret.find(j->first)) != std::string::npos ) {
                        ret.replace(i,j->first.length(),j->second);
                    }
                    j++;
                }
                
                return ret;
                
            }

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

