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

#ifndef JLIB_SYS_SYS_HH
#define JLIB_SYS_SYS_HH

#include <exception>
#include <iostream>
#include <string>

#include <sys/types.h>

#include <functional>
#include <vector>

#include <signal.h>

#include <csignal>
#include <sys/socket.h>

namespace jlib {
    namespace sys {


        class io_exception : public std::exception {
        public:
            io_exception(const std::string& msg = "") {
                m_msg = "io exception: "+msg;
            }
            virtual ~io_exception() {}
            virtual const char* what() const noexcept { return m_msg.c_str(); }
        protected:
            std::string m_msg;
        };
        
        class sys_exception : public std::exception {
        public:
            sys_exception(const std::string& msg = "") {
                m_msg = "sys exception: "+msg;
            }
            virtual ~sys_exception() {}
            virtual const char* what() const noexcept { return m_msg.c_str(); }
        protected:
            std::string m_msg;
        };

        /**
         * read a line from is into s, doing intelligent buffering
         */
        /**
         * Stop writes to this socket from killing the process.
         *
         * Writing to a socket whose peer has closed raises SIGPIPE, and the
         * default disposition of SIGPIPE is to terminate -- so the error return
         * the caller is carefully checking never arrives.  For a mail client
         * that is not an exotic case: a server dropping an idle IMAP connection
         * is routine, and the symptom is the program vanishing without a word.
         *
         * Where the platform has SO_NOSIGPIPE this sets it, which covers every
         * write on the descriptor including the ones OpenSSL makes internally.
         * Where it does not -- Linux -- this does nothing and sigpipe_guard is
         * what does the work.
         */
        void nosigpipe(int fd);

        /**
         * Turn off Nagle's algorithm on a connected socket.
         *
         * **Measured, not assumed.**  Nagle holds a small write until the
         * previous one is acknowledged; the peer's delayed-ACK timer holds
         * that acknowledgement for up to ~40ms when it has nothing to send
         * back yet.  The two together are a stall that only appears once a
         * connection carries *more than one* exchange -- which is why nothing
         * in jlib met it for twenty-six years, and why the HTTP server's
         * keep-alive work met it immediately.
         *
         * It cost a fixed ~44ms per reused TLS connection in the Ubuntu
         * container: 30 requests took 0.051s of which 0.044s was one stall,
         * which made keep-alive look slower than opening 30 connections.  Past
         * the stall the marginal cost is 0.164ms a request against 0.900ms for
         * a fresh TLS connection.
         *
         * Every protocol jlib speaks -- HTTP, IMAP, POP3 -- is request and
         * response, which is the case Nagle's coalescing cannot help and its
         * waiting can only hurt.  The streambuf layer already coalesces: it
         * buffers to BUF_SIZE and writes once.
         *
         * Best effort, like nosigpipe(): a descriptor that will not take the
         * option still works.
         */
        void nodelay(int fd);

        /**
         * Blocks SIGPIPE for the calling thread, and consumes one if it comes.
         *
         * For platforms without SO_NOSIGPIPE, where the alternatives are worse:
         * MSG_NOSIGNAL is per-call and so cannot cover OpenSSL's own writes, and
         * ignoring SIGPIPE process-wide is not a library's decision to make.
         *
         * Blocking it is, since the block is per-thread and undone on the way
         * out.  A SIGPIPE raised while blocked stays pending, so the destructor
         * drains it before unblocking; otherwise it would be delivered to the
         * caller the moment the mask was restored, which is the same crash a
         * little later.
         *
         * A no-op where nosigpipe() has already dealt with it.
         */
        class sigpipe_guard {
        public:
            sigpipe_guard();
            ~sigpipe_guard();

            sigpipe_guard(const sigpipe_guard&) = delete;
            sigpipe_guard& operator=(const sigpipe_guard&) = delete;

        private:
#ifndef SO_NOSIGPIPE
            sigset_t m_old;
            bool m_blocked;   // we are the ones who blocked it
#endif
        };

        /**
         * How long a connect() may take before it is given up on, in seconds.
         *
         * Until this existed every socket in the library connected with no
         * timeout at all, so a host that accepted the SYN and then said nothing
         * -- a firewall configured to drop rather than reject is the ordinary
         * way to get one -- hung the caller until the kernel's own retry limit
         * ran out, which is minutes.  A mail client freezing on startup with no
         * output is exactly that.
         *
         * Zero means wait forever, which is the old behaviour.
         */
        void set_default_connect_timeout(double seconds);
        double get_default_connect_timeout();

        /**
         * How long a read or write on a connected socket may block, in seconds.
         *
         * Zero -- the default, and deliberately so -- means forever.  A read
         * timeout cannot be turned on library-wide without breaking the callers
         * that are *supposed* to sit on a quiet socket: Imap4::idle() waits on
         * an IMAP IDLE for as long as the server keeps it open, which is up to
         * 29 minutes by RFC 2177, and there is no value here that is both long
         * enough for that and short enough to be useful anywhere else.
         *
         * So it is off by default and the caller that knows its own protocol
         * turns it on, either here or per-socket with
         * basic_socketbuf::set_timeout().  The HTTP client does.
         */
        void set_default_io_timeout(double seconds);
        double get_default_io_timeout();

        /**
         * `path` made absolute against the current directory.
         *
         * For anything that will `chdir` later -- see daemon below -- because
         * a relative path resolved after the move means a different file, with
         * no symptom except the wrong one.
         *
         * Purely textual: no `realpath`, because the file need not exist yet.
         * `""` and `"-"` are returned unchanged, being the conventional
         * spellings of "discard" and "standard output" rather than paths.
         */
        std::string absolute_path(const std::string& path);

        /**
         * Who a process should become, resolved from names.
         *
         * Resolved **before** anything is given up: `getpwnam` may reach NSS
         * or LDAP, and a lookup done after the drop fails only on the machines
         * where that service is not a local file.
         */
        struct identity {
            uid_t       uid = 0;
            gid_t       gid = 0;
            std::string user;
            std::string group;
        };

        /**
         * Turn a user and an optional group into ids.
         *
         * An empty group means the user's own primary group.
         *
         * @throws sys_exception naming what could not be found
         */
        identity resolve_identity(const std::string& user,
                                  const std::string& group);

        /**
         * Become that identity, permanently.
         *
         * **The order is the whole of this function**, and each step is a
         * documented way to get privilege dropping wrong:
         *
         *   1. `setgroups()` clears the supplementary groups.  It needs
         *      privilege, so it must come first -- and skipping it is the
         *      classic one: a process that dropped to `www` while keeping
         *      root's supplementary groups has dropped almost nothing.
         *   2. `setgid()` before `setuid()`, because afterwards there is no
         *      privilege left to change group with.  The reverse order
         *      compiles, runs, looks identical, and leaves the process in the
         *      group it started in.
         *   3. `setuid()` last, then verify_identity().
         *
         * Returns without doing anything when the process is already that
         * identity: there is nothing to give up, and `setgroups` would fail
         * for no reason.
         *
         * @throws sys_exception if any step fails or the check does not hold
         */
        void become(const identity& who);

        /**
         * Is this process that identity, with no way back?
         *
         * Separate because **it is the step implementations skip.**
         * `setuid()` can fail and return an error nobody reads, leaving a
         * process that believes it is unprivileged and is not.  Checks the
         * real *and* effective ids, then tries to regain root, which must
         * fail: a drop you can undo is not a drop.
         *
         * Separate also because it can be tested without privilege, while the
         * three syscalls above cannot.
         *
         * @throws sys_exception saying which half failed
         */
        void verify_identity(const identity& who);

        /**
         * Fork into the background, and tell the caller whether it worked.
         *
         * ## Why a pipe
         *
         * A daemon that forks and *then* fails -- a port in use, a file it
         * cannot read -- has already handed the shell a zero exit, and every
         * script that starts it believes it is running.  So the child keeps
         * one end of a pipe and the parent blocks on the other: the parent
         * does not exit until the child says it is ready, and if the child
         * dies first the parent sees the pipe close and exits non-zero.
         *
         * **stderr is left alone until ready().**  The child inherits the
         * terminal, so every failure path a caller already has keeps printing
         * where somebody can read it, and the pipe carries only *whether* to
         * exit zero.  That is what makes this small rather than a rework of
         * every error return in the calling program.
         *
         * ## Why twice
         *
         * The first fork lets the parent return to the shell; `setsid()` makes
         * the child a session leader with no controlling terminal.  A session
         * leader can *acquire* one by opening a terminal device, so it forks
         * again and continues as a grandchild, which cannot.  The second fork
         * is about what could happen later rather than what has happened.
         *
         * ## Threads
         *
         * **Call start() before anything creates a thread.**  `fork()` keeps
         * only the calling thread, and a mutex another thread held stays
         * locked forever in the child.  A server with a pool or a log writer
         * has to daemonise before it builds either.
         */
        class daemon {
        public:
            /**
             * Fork.  In the parent this **does not return** -- it waits for
             * the child's verdict and exits with it.
             *
             * @throws sys_exception if the pipe or either fork fails
             */
            void start();

            /**
             * Tell the parent to exit zero, and let go of the terminal.
             *
             * Called once the caller is actually serving.  Does nothing if
             * start() was never called, so a program can carry one of these
             * unconditionally and only daemonise when asked.
             */
            void ready();

        private:
            int m_tell = -1;
        };

        void getline(std::istream& is, std::string& s);

        /**
         * read a std::string n bytes long from is into s, doing intelligent buffering
         * if n is -1, read to the end of the stream
         */
        void getstring(std::istream& is, std::string& s, int n=-1);
        void read(std::istream& is, std::string& s, int n=-1);
        void read(std::istream& is, char* c, int n);


        /**
         * call the method passed in s in a new thread, then
         * kill the thread
         *
         * if s isn't "", then lock the global mutex for that key
         * before and unlock after
         *
         */
        void thread(const std::function<void()>& slot, const std::string& s="");

        /**
         * lock a global mutex referred to by s
         */
        void lock(const std::string& s);

        /**
         * lock a global mutex referred to by s
         */
        void unlock(const std::string& s);

        /**
         * is the mutex locked?
         */
        bool locked(const std::string& s);

        /**
         * Run a program.  No shell is involved.
         *
         * argv[0] is the program, looked up on PATH, and the rest are its
         * arguments exactly as written -- a space, a quote, a semicolon or a
         * backtick in one of them is a character in an argument and nothing
         * else.  Its standard output and standard error come back in out and
         * err, and the return value is the exit status.
         *
         *     sys::run({ "file", "--mime-type", "-b", path }, out, err);
         *
         * This is what to reach for.  shell() below builds a command line and
         * hands it to /bin/sh, so every caller of it that interpolates a
         * string it did not choose is an injection: jlib had five, and two of
         * them were "rm" and "mv" on a folder name that came from a mail
         * server.
         *
         * Throws sys::exception if the program could not be started.  A
         * program that ran and failed is not an exception -- that is what the
         * status is for.
         */
        int run(const std::vector<std::string>& argv, std::string& out, std::string& err);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails
         *
         * The string is parsed by /bin/sh, so anything interpolated into it
         * is code.  Use run() unless a shell is what is actually wanted.
         */
        void shell(const std::string& cmd);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails.  return stdout and stderr in the passed
         * strings.
         */
        void shell(const std::string& cmd, std::string& out, std::string& err);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails.  return stdout and stderr in the passed
         * strings.
         *
         * this version of the function also allows you to pass data into 
         * the shell command using stdin.  The flag input_file tells whether
         * the parameter 'in' is a file path or a plain string.
         */
        void shell(const std::string& cmd, const std::string& in, std::string& out, std::string& err, bool in_file=true);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails.  return stdout and stderr in the passed
         * strings. The tmp files used by this function are reasonably
         * secure, in that they will be overwritten before deletion
         * to keep anyone from recovering the contents.
         *
         */
        void secure_shell(const std::string& cmd, std::string& out, std::string& err);

        /**
         * run the std::string as a shell command, and throw an exception
         * if the command fails.  return stdout and stderr in the passed
         * strings.  The tmp files used by this function are reasonably
         * secure, in that they will be overwritten before deletion
         * to keep anyone from recovering the contents.
         *
         * this version of the function also allows you to pass data into 
         * the shell command using stdin.  The flag input_file tells whether
         * the parameter 'in' is a file path or a plain string.
         */
        void secure_shell(const std::string& cmd, const std::string& in, std::string& out, std::string& err, bool in_file=false);

        /**
         * A self-pipe, so a signal handler can hand work to an ordinary thread.
         *
         * **What a handler may do is very short**, and almost nothing a
         * program wants to do on SIGINT is on the list. Stopping a server
         * takes locks and allocates; writing a message uses iostreams;
         * re-reading a config file does I/O. A handler that does any of them
         * can deadlock against the thread it interrupted -- if that thread
         * already holds the mutex the handler now wants, nothing will ever
         * release it.
         *
         * `write()` *is* on the list, which is the whole trick: the handler
         * writes one byte and returns, a thread blocked on the other end wakes
         * up, and that thread may do anything it likes.
         *
         * Static because a handler takes no context beyond its signal number,
         * and a process has one signal disposition. Nothing here is a class
         * you instantiate; it is the process's one wakeup.
         *
         * **Why a thread and not the reactor.** A reactor already multiplexes
         * descriptors, and registering the read end with one would save the
         * thread -- for a signal whose handling is quick. `stop()` is such a
         * signal: it is explicitly safe on the reactor thread, which is where
         * it would then run.
         *
         * SIGHUP is not. Re-reading a config file, loading a certificate and
         * hashing a decoy with Argon2id all block, and a reactor callback that
         * blocks stalls every connection the server is holding. So a program
         * that reloads needs a thread whatever else it does -- and once it has
         * one, giving the same thread the stop signal is one mechanism rather
         * than two.
         *
         * The thread costs a stack. It is blocked in read(), not polling.
         *
         *     sys::wakeup::arm();
         *     std::signal(SIGINT, sys::wakeup::on_signal);
         *
         *     // ...on a thread of its own:
         *     while(sys::wakeup::wait()) {
         *         if(sys::wakeup::count(SIGINT) != acted) { acted = ...; stop(); }
         *     }
         */
        class wakeup {
        public:
            /**
             * Open the pipe.  Call before installing any handler.
             *
             * Both ends close across exec and the write end never blocks: a
             * full pipe means a wakeup nobody has read, which is what was
             * wanted anyway, and a handler that blocked on it could stop the
             * very thread that drains it.
             *
             * @return false if the pipe could not be made
             */
            static bool arm();

            /**
             * A handler: count the signal and poke the pipe.
             *
             * Async-signal-safe, and it restores errno -- it can interrupt a
             * thread between a failing syscall and its errno check, and the
             * write() in here would otherwise overwrite the value that thread
             * is about to read.
             */
            static void on_signal(int sig);

            /** Why wait() came back. */
            enum class woken {
                signalled,   ///< at least one signal arrived
                poked,       ///< poke() only; no signal behind it
                closed       ///< the pipe is gone, and nothing more will come
            };

            /**
             * Block until a signal arrives or somebody pokes.
             *
             * **Three answers rather than two, because two was a trap.** A
             * bool cannot separate "a signal arrived" from "somebody poked",
             * so every looping caller had to carry a flag saying which it had
             * asked for -- and forgetting it means waking on the shutdown poke,
             * finding nothing, and blocking again while a join waits forever.
             * That bug was written here, in the second of two callers, by the
             * author of the first. An API whose misuse is invisible in every
             * manual test and fatal in production is worth the wider return
             * type.
             *
             * `closed` was the old `false`, and separating it is worth
             * something on its own: a caller can now tell an orderly shutdown
             * from a pipe that broke.
             *
             * **This does not replace count().** The enum says why you woke;
             * the counts say how many signals you have not acted on yet, and
             * one wakeup can carry several.
             */
            static woken wait();

            /**
             * Wake a waiter without a signal, to shut it down.
             *
             * wait() answers `poked` for this and `signalled` for a signal,
             * so a caller can tell them apart. It did not always: the return
             * was a bool, every looping caller had to carry a flag saying
             * which it had asked for, and the one that forgot hung a join
             * forever.
             */
            static void poke();

            /** Close the pipe, ending any wait(). */
            static void disarm();

            /**
             * How many times `sig` has arrived.
             *
             * A count rather than a flag a reader clears, so a signal landing
             * between the read and the clear cannot be lost. A caller keeps
             * the value it last acted on and compares.
             */
            static std::sig_atomic_t count(int sig);
        };

    }
}
#endif //JLIB_SYS_SYS_HH
