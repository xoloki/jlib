/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2000 Joe Yandle <jwy@divisionbyzero.com>
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

#include <jlib/sys/sys.hh>
#include <jlib/sys/tfstream.hh>

#include <map>

#include <cstdlib>
#include <cstring>


const int SZ = 1024;

namespace jlib {
    namespace sys {
        
        static std::map<std::string,pthread_mutex_t*> g_mutex;
        
        void getline(std::istream& is, std::string& s) {
            std::getline(is,s);
            s.erase(s.find_last_not_of("\r")+1);
        }
        
        void getstring(std::istream& is, std::string& s, int n) {
            return read(is,s,n);
        }

        void read(std::istream& is, std::string& s, int n) {
            int count = 0;
            char buf[SZ];
            int amt,diff,num;
            
            s.clear();

            while((n==-1 ||count<n) && !is.eof()) {
                amt = SZ-1;
                if(n != -1) {
                    diff = n-count;
                    if(amt > diff) {
                        amt = diff;
                    }
                }
                is.read(buf,amt);
                if(is.bad())
                    throw io_exception("bad() istream in jlib::sys::getstring");
                num=is.gcount();
                count += num;
                s.append(buf,num);
            }
        }
        
        void read(std::istream& is, char* c, int n) {
            int count = 0;
            int amt,diff,num;
            
            while((n==-1 ||count<n) && !is.eof()) {
                amt = SZ-1;
                if(n != -1) {
                    diff = n-count;
                    if(amt > diff) {
                        amt = diff;
                    }
                }
                is.read(c+count,amt);
                if(is.bad())
                    throw io_exception("bad() istream in jlib::sys::getstring");
                num=is.gcount();
                count += num;
            }
        }

        typedef struct _slot_string {
            sigc::slot0<void> slot;
            std::string mutex;
        } slot_string;

        void thread_callback(void* data) {
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "entering jlib::sys::thread_callback()"<<std::endl;
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "slot_string* ss = reinterpret_cast<slot_string*>(data)"<<std::endl;
            slot_string* ss = reinterpret_cast<slot_string*>(data);
            //sigc::slot0<void>* ss = reinterpret_cast<sigc::slot0<void>*>(data);

            if(ss->mutex != "") {
                if(std::getenv("JLIB_SYS_DEBUG"))
                    std::cout << "lock(\""<<ss->mutex<<"\")"<<std::endl;
                lock(ss->mutex);
            }

            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "ss->slot()"<<std::endl;
            try {
                ss->slot();
            }
            catch(std::exception& e) {
                std::cerr << "exception while running s->call() in jlib::sys::thread_callback" << std::endl
                     << e.what() << std::endl;
            }
            catch(...) {
                std::cerr << "unknown exception while running s->call() in jlib::sys::thread_callback" << std::endl;
            }

            if(ss->mutex != "") {
                if(std::getenv("JLIB_SYS_DEBUG"))
                    std::cout << "unlock(\""<<ss->mutex<<"\")"<<std::endl;
                unlock(ss->mutex);
            }
            
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "delete ss"<<std::endl;
            delete ss;
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "leaving jlib::sys::thread_callback()"<<std::endl;
            pthread_exit(0);
        }

        

        void thread(const sigc::slot0<void>& slt, std::string s) {
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "entering jlib::sys::thread()"<<std::endl;

            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "creating slot_string*"<<std::endl;
            slot_string* ss = new slot_string;
            //sigc::slot0<void>* ss = new sigc::slot0<void>(slt);
            //ss->slot = new sigc::slot0<void>(slt);
            ss->slot = slt;
            ss->mutex = s;
            
            pthread_t thread;
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "calling pthread_create()"<<std::endl;
            pthread_create(&thread, 0, reinterpret_cast<void * (*)(void *)>(&thread_callback), reinterpret_cast<void*>(ss));
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "calling pthread_detach()"<<std::endl;
            pthread_detach(thread);
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "leaving jlib::sys::thread()"<<std::endl;
        }
        
        void lock(std::string s) {
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "entering jlib::sys::lock(\""<<s<<"\")"<<std::endl;
            if(g_mutex.find(s) == g_mutex.end()) {
                if(std::getenv("JLIB_SYS_DEBUG"))
                    std::cout << "didn't find mutex, creating it now" << std::endl;
                g_mutex[s] = new pthread_mutex_t;
                pthread_mutex_init(g_mutex[s], NULL);
            }
            if(pthread_mutex_lock(g_mutex[s]))
                std::cerr << "error locking g_mutex[\""<<s<<"\"]"<<std::endl;
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "leaving jlib::sys::lock(\""<<s<<"\")"<<std::endl;
        }
        
        void unlock(std::string s) {
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "entering jlib::sys::unlock(\""<<s<<"\")"<<std::endl;
            if(g_mutex.find(s) != g_mutex.end()) {
                if(std::getenv("JLIB_SYS_DEBUG"))
                    std::cout << "found mutex, unlocking"<<std::endl;
                if(pthread_mutex_unlock(g_mutex[s]))
                    std::cerr << "error unlocking g_mutex[\""<<s<<"\"]"<<std::endl;
            }
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "leaving jlib::sys::unlock(\""<<s<<"\")"<<std::endl;
        }

        bool locked(std::string s) {
            if(g_mutex.find(s) == g_mutex.end()) {
                g_mutex[s] = new pthread_mutex_t;
                pthread_mutex_init(g_mutex[s], NULL);
            }
            if(pthread_mutex_trylock(g_mutex[s])) {
                return true;
            }
            else {
                pthread_mutex_unlock(g_mutex[s]);
                return false;
            }
        }

        void shell(std::string cmd) {
            tfstream stderrstr;
            std::string err;
            cmd = cmd + " 2>"+stderrstr.get_path();
            int ret = system(cmd.c_str());
            stderrstr.seekg(0,std::ios_base::beg);
            getstring(stderrstr,err);

            if(ret != 0) {
                throw sys_exception("error running shell command '"+cmd+"': '"+err+"'");
            }
        }

        void shell(std::string cmd, std::string& out, std::string& err) {
            tfstream stdoutstr, stderrstr;
            cmd = cmd + " >"+stdoutstr.get_path()+" 2>"+stderrstr.get_path();
            int ret = system(cmd.c_str());

            stdoutstr.seekg(0,std::ios_base::beg);
            stderrstr.seekg(0,std::ios_base::beg);
            
            getstring(stdoutstr,out);
            getstring(stderrstr,err);

            if(ret != 0) {
                throw sys_exception("error running shell command '"+cmd+"': '"+err+"'");
            }
        }

        void shell(std::string cmd, std::string in, std::string& out, std::string& err, bool in_file) {
            tfstream stdinstr, stdoutstr, stderrstr;
            if(in_file) {
                stdinstr.close();
                cmd = cmd+" <"+in+" >"+stdoutstr.get_path()+" 2>"+stderrstr.get_path();
            }
            else {
                stdinstr << in;
                stdinstr.close();
                cmd = cmd+" <"+stdinstr.get_path()+" >"+stdoutstr.get_path()+" 2>"+stderrstr.get_path();
            }
            int ret = system(cmd.c_str());
            stdoutstr.seekg(0,std::ios_base::beg);
            stderrstr.seekg(0,std::ios_base::beg);
            
            getstring(stdoutstr,out);
            getstring(stderrstr,err);

            if(ret != 0) {
                throw sys_exception("error running shell command '"+cmd+"': '"+err+"'");
            }
        }

        void secure_shell(std::string cmd, std::string& out, std::string& err) {
            stfstream stdoutstr, stderrstr;
            cmd = cmd + " >"+stdoutstr.get_path()+" 2>"+stderrstr.get_path();
            int ret = system(cmd.c_str());
            stdoutstr.seekg(0,std::ios_base::beg);
            stderrstr.seekg(0,std::ios_base::beg);
            
            getstring(stdoutstr,out);
            getstring(stderrstr,err);

            if(ret != 0) {
                throw sys_exception("error running shell command '"+cmd+"': '"+err+"'");
            }
        }

        void secure_shell(std::string cmd, std::string in, std::string& out, std::string& err, bool in_file) {
            stfstream stdinstr, stdoutstr, stderrstr;
            if(in_file) {
                stdinstr.close();
                cmd = cmd+" <"+in+" >"+stdoutstr.get_path()+" 2>"+stderrstr.get_path();
            }
            else {
                stdinstr << in;
                stdinstr.close();
                cmd = cmd+" <"+stdinstr.get_path()+" >"+stdoutstr.get_path()+" 2>"+stderrstr.get_path();
            }
            int ret = system(cmd.c_str());
            stdoutstr.seekg(0,std::ios_base::beg);
            stderrstr.seekg(0,std::ios_base::beg);
            
            getstring(stdoutstr,out);
            getstring(stderrstr,err);

            if(ret != 0) {
                throw sys_exception("error running shell command '"+cmd+"': '"+err+"'");
            }
        }


    }
}

