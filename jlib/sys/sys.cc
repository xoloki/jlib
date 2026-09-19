/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2000 Joey Yandle <xoloki@gmail.com>
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

#include <jlib/sys/sys.hh>

#include <pthread.h>
#include <cerrno>
#include <jlib/sys/tfstream.hh>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <grp.h>
#include <pwd.h>
#include <limits.h>
#include <unistd.h>

#include <fstream>
#include <vector>


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

        namespace {

            // Not a mutex-guarded pair: these are set once at startup if they
            // are set at all, and an atomic read on every connect is cheaper
            // than a lock.
            std::atomic<double> g_connect_timeout{30.0};
            std::atomic<double> g_io_timeout{0.0};

        }

        void set_default_connect_timeout(double seconds) {
            g_connect_timeout.store(seconds < 0 ? 0 : seconds);
        }

        double get_default_connect_timeout() {
            return g_connect_timeout.load();
        }

        void set_default_io_timeout(double seconds) {
            g_io_timeout.store(seconds < 0 ? 0 : seconds);
        }

        double get_default_io_timeout() {
            return g_io_timeout.load();
        }

        void nodelay(int fd) {
            int on = 1;

            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
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

        std::string absolute_path(const std::string& path) {
            if(path.empty() || path == "-" || path[0] == '/') return path;

            char here[PATH_MAX];

            if(::getcwd(here, sizeof here) == 0) return path;

            return std::string(here) + "/" + path;
        }

        identity resolve_identity(const std::string& user,
                                  const std::string& group)
        {
            identity who;

            who.user = user;
            who.group = group;

            const struct passwd* pw = ::getpwnam(user.c_str());

            if(pw == 0)
                throw sys_exception("no such user: \"" + user + "\"");

            who.uid = pw->pw_uid;
            who.gid = pw->pw_gid;

            if(!group.empty()) {
                const struct group* gr = ::getgrnam(group.c_str());

                if(gr == 0)
                    throw sys_exception("no such group: \"" + group + "\"");

                who.gid = gr->gr_gid;
            }

            return who;
        }

        void verify_identity(const identity& who) {
            if(::getuid() != who.uid || ::geteuid() != who.uid ||
               ::getgid() != who.gid || ::getegid() != who.gid)
            {
                throw sys_exception("the privilege drop did not take: wanted "
                                    "uid " + std::to_string(who.uid) + " gid " +
                                    std::to_string(who.gid) + ", have uid " +
                                    std::to_string(::getuid()) + " gid " +
                                    std::to_string(::getgid()));
            }

            // Only when the target is not root, because a process told to run
            // as root can obviously become root.
            if(who.uid != 0 && ::setuid(0) == 0)
                throw sys_exception("root was regained after dropping it");
        }

        void become(const identity& who) {
            // Already there: nothing to give up, and nothing that needs
            // privilege to do it with.
            if(::getuid() == who.uid && ::getgid() == who.gid) return;

            if(::setgroups(1, &who.gid) != 0) {
                throw sys_exception("setgroups() failed: " +
                                    std::string(std::strerror(errno)));
            }

            if(::setgid(who.gid) != 0) {
                throw sys_exception("setgid() failed: " +
                                    std::string(std::strerror(errno)));
            }

            if(::setuid(who.uid) != 0) {
                throw sys_exception("setuid() failed: " +
                                    std::string(std::strerror(errno)));
            }

            verify_identity(who);
        }

        void daemon::start() {
            int fds[2];

            if(::pipe(fds) != 0) {
                throw sys_exception("pipe() for the readiness signal failed: " +
                                    std::string(std::strerror(errno)));
            }

            const pid_t first = ::fork();

            if(first < 0) {
                throw sys_exception("fork() failed: " +
                                    std::string(std::strerror(errno)));
            }

            if(first > 0) {
                // The parent, which never returns from here.
                ::close(fds[1]);

                char answer = 0;

                const ssize_t got = ::read(fds[0], &answer, 1);

                ::close(fds[0]);

                // EOF means the child went away without saying it was ready,
                // and it will have printed why on the terminal we share.
                ::_exit(got == 1 && answer == 'k' ? 0 : 1);
            }

            ::close(fds[0]);

            if(::setsid() < 0) {
                throw sys_exception("setsid() failed: " +
                                    std::string(std::strerror(errno)));
            }

            // The second fork: see the note on the class.  The intermediate
            // exits at once; the grandchild keeps the write end, so the parent
            // does not yet read EOF.
            const pid_t second = ::fork();

            if(second < 0) {
                throw sys_exception("the second fork() failed: " +
                                    std::string(std::strerror(errno)));
            }

            if(second > 0) ::_exit(0);

            m_tell = fds[1];

            // Away from wherever it started, so it does not hold a mount busy.
            // Every path a caller cares about must already be absolute --
            // see absolute_path().
            if(::chdir("/") != 0) { }
        }

        void daemon::ready() {
            if(m_tell < 0) return;

            const char ok = 'k';

            if(::write(m_tell, &ok, 1) != 1) { }

            ::close(m_tell);

            m_tell = -1;

            // Only now: until this point every failure path printed to the
            // terminal the parent shares, which is what makes a failed start
            // visible.
            const int null = ::open("/dev/null", O_RDWR);

            if(null >= 0) {
                ::dup2(null, STDIN_FILENO);
                ::dup2(null, STDOUT_FILENO);
                ::dup2(null, STDERR_FILENO);

                if(null > STDERR_FILENO) ::close(null);
            }
        }

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

        int run(const std::vector<std::string>& argv, std::string& out, std::string& err) {
            if(argv.empty()) {
                throw sys_exception("jlib::sys::run() with no program to run");
            }

            // Output goes to temp files rather than pipes, which is what
            // shell() does and avoids the deadlock a pair of pipes needs a
            // poll loop to escape: a child that fills the stderr pipe while
            // the parent is still reading stdout blocks forever.
            tfstream outf, errf;
            const std::string outp = outf.get_path(), errp = errf.get_path();

            outf.close();
            errf.close();

            std::vector<char*> args;

            for(const std::string& a : argv) {
                args.push_back(const_cast<char*>(a.c_str()));
            }

            args.push_back(nullptr);

            const pid_t pid = fork();

            if(pid < 0) {
                throw sys_exception("fork() in jlib::sys::run(): " +
                                    std::string(std::strerror(errno)));
            }

            if(pid == 0) {
                // The child.  Nothing here may throw or allocate in a way that
                // matters -- between fork and exec, only async-signal-safe
                // calls are defined in a process that might have threads.
                const int o = ::open(outp.c_str(), O_WRONLY | O_TRUNC);
                const int e = ::open(errp.c_str(), O_WRONLY | O_TRUNC);

                if(o >= 0) { ::dup2(o, STDOUT_FILENO); ::close(o); }
                if(e >= 0) { ::dup2(e, STDERR_FILENO); ::close(e); }

                ::execvp(args[0], args.data());

                // Only reached if exec failed.  _exit, not exit: the child
                // shares the parent's stdio buffers and must not flush them.
                ::_exit(127);
            }

            int status = 0;

            while(::waitpid(pid, &status, 0) < 0) {
                if(errno != EINTR) {
                    throw sys_exception("waitpid() in jlib::sys::run(): " +
                                        std::string(std::strerror(errno)));
                }
            }

            std::ifstream ofs(outp.c_str()), efs(errp.c_str());

            getstring(ofs, out);
            getstring(efs, err);

            if(WIFSIGNALED(status)) {
                return 128 + WTERMSIG(status);
            }

            const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

            if(code == 127 && out.empty() && err.empty()) {
                throw sys_exception("could not run \"" + argv[0] + "\"");
            }

            return code;
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


    
// ------------------------------------------------------------------ wakeup

namespace {

    /**
     * The write end, for the handler.
     *
     * `sig_atomic_t` rather than `int` because a handler may read it while an
     * ordinary thread is still writing it, and that is the one type the
     * standard says can be touched from a handler without a data race.
     */
    volatile std::sig_atomic_t wake_write = -1;

    /** The read end.  Only ordinary threads look at this. */
    int wake_read = -1;

    /**
     * One counter per signal.
     *
     * NSIG is the number of signals the platform defines; a handler is only
     * ever called with one of those, and the bound is checked anyway because
     * an out-of-range write here would be a memory error inside a signal
     * handler, which is the worst place to have one.
     */
    volatile std::sig_atomic_t wake_count[NSIG] = { 0 };

}

bool wakeup::arm() {
    if(wake_read >= 0) return true;

    int fd[2];

    if(::pipe(fd) != 0) return false;

    ::fcntl(fd[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fd[1], F_SETFD, FD_CLOEXEC);
    ::fcntl(fd[1], F_SETFL, ::fcntl(fd[1], F_GETFL, 0) | O_NONBLOCK);

    wake_read = fd[0];
    wake_write = fd[1];

    return true;
}

void wakeup::on_signal(int sig) {
    const int kept = errno;

    if(sig >= 0 && sig < NSIG) wake_count[sig]++;

    if(wake_write >= 0) {
        const char one = 'w';

        // Discarded on purpose: EAGAIN means a wakeup is already pending,
        // which is exactly what this wanted.
        const ssize_t n = ::write(static_cast<int>(wake_write), &one, 1);

        (void)n;
    }

    errno = kept;
}

wakeup::woken wakeup::wait() {
    if(wake_read < 0) return woken::closed;

    for(;;) {
        char drain[64];
        const ssize_t n = ::read(wake_read, drain, sizeof drain);

        if(n > 0) {
            // **Scanned, not looked at, and a signal wins.**
            //
            // One read drains whatever has accumulated, so a signal and a
            // poke that arrive together come back in the same buffer -- "wp",
            // measured, not assumed. Deciding from drain[0] or from the last
            // byte is then a coin flip, and losing the coin means answering
            // `poked` while a signal is in hand: the caller returns without
            // acting on it, and Ctrl-C does nothing at all. That is worse than
            // the hang the enum exists to prevent, and much harder to see.
            for(ssize_t i = 0; i < n; i++) {
                if(drain[i] == 'w') return woken::signalled;
            }

            return woken::poked;
        }

        // A signal arriving during the read is not a reason to give up -- it
        // is very often the signal being waited for, whose handler has just
        // written the byte the retry will find.
        if(n < 0 && errno == EINTR) continue;

        return woken::closed;
    }
}

void wakeup::poke() {
    if(wake_write < 0) return;

    const char one = 'p';
    const ssize_t n = ::write(static_cast<int>(wake_write), &one, 1);

    (void)n;
}

void wakeup::disarm() {
    const int r = wake_read;
    const int w = static_cast<int>(wake_write);

    // The write end first, so a handler that runs between these two lines
    // writes to a closed descriptor and fails harmlessly rather than into a
    // number the process has since reused for something else.
    wake_write = -1;
    wake_read = -1;

    if(w >= 0) ::close(w);
    if(r >= 0) ::close(r);
}

std::sig_atomic_t wakeup::count(int sig) {
    if(sig < 0 || sig >= NSIG) return 0;

    return wake_count[sig];
}

}
}