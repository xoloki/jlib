/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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

#include <pthread.h>
#include <cerrno>
#include <jlib/sys/tfstream.hh>

#include <map>
#include <mutex>
#include <thread>

#include <cstdlib>
#include <cstring>


const int SZ = 1024;

namespace jlib {
    namespace sys {

        namespace {

            // The named-mutex registry used to be a std::map of raw
            // pthread_mutex_t* mutated without synchronization from lock() and
            // locked() -- a data race in an API whose whole purpose is to be
            // called from several threads.  The mutexes were also never freed.
            // std::map guarantees reference stability, so handing back a
            // reference after dropping the registry lock is safe.
            std::mutex g_registry_lock;
            std::map<std::string, std::mutex> g_registry;

            std::mutex& named_mutex(const std::string& s) {
                std::lock_guard<std::mutex> guard(g_registry_lock);
                return g_registry[s];
            }

            std::mutex* find_named_mutex(const std::string& s) {
                std::lock_guard<std::mutex> guard(g_registry_lock);
                std::map<std::string, std::mutex>::iterator i = g_registry.find(s);
                return i == g_registry.end() ? nullptr : &i->second;
            }

        }

        void nosigpipe(int fd) {
#ifdef SO_NOSIGPIPE
            int on = 1;

            // Best effort: a socket that will not take the option still works,
            // it just leaves the job to sigpipe_guard, and failing the connect
            // over it would be worse than the problem.
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
            (void)fd;
#endif
        }

#ifdef SO_NOSIGPIPE

        sigpipe_guard::sigpipe_guard() {}
        sigpipe_guard::~sigpipe_guard() {}

#else

        sigpipe_guard::sigpipe_guard()
            : m_blocked(false)
        {
            sigset_t pipe_only;
            sigemptyset(&pipe_only);
            sigaddset(&pipe_only, SIGPIPE);

            if(pthread_sigmask(SIG_BLOCK, &pipe_only, &m_old) != 0)
                return;

            // Only unblock on the way out if it was us who blocked it; a caller
            // that had already blocked SIGPIPE keeps its own arrangement, and
            // its pending signal is not ours to consume.
            m_blocked = !sigismember(&m_old, SIGPIPE);
        }

        sigpipe_guard::~sigpipe_guard() {
            if(!m_blocked)
                return;

            sigset_t pending;
            sigemptyset(&pending);

            if(sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
                sigset_t pipe_only;
                sigemptyset(&pipe_only);
                sigaddset(&pipe_only, SIGPIPE);

                // Take it off the pending set before unblocking, or it lands on
                // the caller the instant the mask is restored -- the same
                // termination, just later and harder to explain.
                const struct timespec none = { 0, 0 };
                while(sigtimedwait(&pipe_only, 0, &none) == -1 && errno == EINTR)
                    ;
            }

            pthread_sigmask(SIG_SETMASK, &m_old, 0);
        }

#endif

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

        // This used to hand pthread_create a void(void*) function reinterpret_cast
        // to void*(*)(void*) and call through it, which is undefined behaviour,
        // and hand-managed the slot with new/delete.  std::thread does both
        // correctly and needs neither.
        void thread(const std::function<void()>& slt, const std::string& s) {
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "entering jlib::sys::thread()"<<std::endl;

            std::thread worker([slt, mutex = std::move(s)]() {
                if(std::getenv("JLIB_SYS_DEBUG"))
                    std::cout << "entering jlib::sys::thread worker"<<std::endl;

                if(mutex != "") {
                    if(std::getenv("JLIB_SYS_DEBUG"))
                        std::cout << "lock(\""<<mutex<<"\")"<<std::endl;
                    lock(mutex);
                }

                try {
                    slt();
                }
                catch(std::exception& e) {
                    std::cerr << "exception while running slot in jlib::sys::thread" << std::endl
                         << e.what() << std::endl;
                }
                catch(...) {
                    std::cerr << "unknown exception while running slot in jlib::sys::thread" << std::endl;
                }

                if(mutex != "") {
                    if(std::getenv("JLIB_SYS_DEBUG"))
                        std::cout << "unlock(\""<<mutex<<"\")"<<std::endl;
                    unlock(mutex);
                }

                if(std::getenv("JLIB_SYS_DEBUG"))
                    std::cout << "leaving jlib::sys::thread worker"<<std::endl;
            });

            worker.detach();

            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "leaving jlib::sys::thread()"<<std::endl;
        }
        
        void lock(const std::string& s) {
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "entering jlib::sys::lock(\""<<s<<"\")"<<std::endl;
            named_mutex(s).lock();
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "leaving jlib::sys::lock(\""<<s<<"\")"<<std::endl;
        }
        
        void unlock(const std::string& s) {
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "entering jlib::sys::unlock(\""<<s<<"\")"<<std::endl;
            // As before, unlocking a name that was never locked is a no-op
            // rather than an error.
            if(std::mutex* m = find_named_mutex(s)) {
                if(std::getenv("JLIB_SYS_DEBUG"))
                    std::cout << "found mutex, unlocking"<<std::endl;
                m->unlock();
            }
            if(std::getenv("JLIB_SYS_DEBUG"))
                std::cout << "leaving jlib::sys::unlock(\""<<s<<"\")"<<std::endl;
        }

        bool locked(const std::string& s) {
            std::mutex& m = named_mutex(s);
            if(m.try_lock()) {
                m.unlock();
                return false;
            }
            return true;
        }

        void shell(const std::string& cmd) {
            tfstream stderrstr;
            std::string err;
            const std::string line = cmd + " 2>"+stderrstr.get_path();
            int ret = system(line.c_str());
            stderrstr.seekg(0,std::ios_base::beg);
            getstring(stderrstr,err);

            if(ret != 0) {
                throw sys_exception("error running shell command '"+line+"': '"+err+"'");
            }
        }

        void shell(const std::string& cmd, std::string& out, std::string& err) {
            tfstream stdoutstr, stderrstr;
            const std::string line = cmd + " >"+stdoutstr.get_path()+" 2>"+stderrstr.get_path();
            int ret = system(line.c_str());

            stdoutstr.seekg(0,std::ios_base::beg);
            stderrstr.seekg(0,std::ios_base::beg);
            
            getstring(stdoutstr,out);
            getstring(stderrstr,err);

            if(ret != 0) {
                throw sys_exception("error running shell command '"+line+"': '"+err+"'");
            }
        }

        void shell(const std::string& cmd, const std::string& in, std::string& out, std::string& err, bool in_file) {
            tfstream stdinstr, stdoutstr, stderrstr;
            std::string line;

            if(in_file) {
                stdinstr.close();
                line = cmd + " <"+in+" >"+stdoutstr.get_path()+" 2>"+stderrstr.get_path();
            }
            else {
                stdinstr << in;
                stdinstr.close();
                line = cmd + " <"+stdinstr.get_path()+" >"+stdoutstr.get_path()+" 2>"+stderrstr.get_path();
            }
            int ret = system(line.c_str());
            stdoutstr.seekg(0,std::ios_base::beg);
            stderrstr.seekg(0,std::ios_base::beg);
            
            getstring(stdoutstr,out);
            getstring(stderrstr,err);

            if(ret != 0) {
                throw sys_exception("error running shell command '"+line+"': '"+err+"'");
            }
        }

        void secure_shell(const std::string& cmd, std::string& out, std::string& err) {
            stfstream stdoutstr, stderrstr;
            const std::string line = cmd + " >"+stdoutstr.get_path()+" 2>"+stderrstr.get_path();
            int ret = system(line.c_str());
            stdoutstr.seekg(0,std::ios_base::beg);
            stderrstr.seekg(0,std::ios_base::beg);
            
            getstring(stdoutstr,out);
            getstring(stderrstr,err);

            if(ret != 0) {
                throw sys_exception("error running shell command '"+line+"': '"+err+"'");
            }
        }

        void secure_shell(const std::string& cmd, const std::string& in, std::string& out, std::string& err, bool in_file) {
            stfstream stdinstr, stdoutstr, stderrstr;
            std::string line;

            if(in_file) {
                stdinstr.close();
                line = cmd + " <"+in+" >"+stdoutstr.get_path()+" 2>"+stderrstr.get_path();
            }
            else {
                stdinstr << in;
                stdinstr.close();
                line = cmd + " <"+stdinstr.get_path()+" >"+stdoutstr.get_path()+" 2>"+stderrstr.get_path();
            }
            int ret = system(line.c_str());
            stdoutstr.seekg(0,std::ios_base::beg);
            stderrstr.seekg(0,std::ios_base::beg);
            
            getstring(stdoutstr,out);
            getstring(stderrstr,err);

            if(ret != 0) {
                throw sys_exception("error running shell command '"+line+"': '"+err+"'");
            }
        }


    }
}

