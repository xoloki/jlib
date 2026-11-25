/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_TESTS_PTY_HH
#define JLIB_TESTS_PTY_HH

/**
 * A pseudo-terminal, so a curses program can be driven by `make check`.
 *
 * `sys::pstream` drives a subprocess down a pipe, and the first draft of this
 * comment claimed a curses program refuses to start without a terminal. It
 * does not, and the difference is worth stating because it decides what the
 * harness is actually for. Measured, same program both ways:
 *
 *                      down a pipe        through this
 *     initscr()        succeeds           succeeds
 *     LINES, COLS      24 by 80           whatever was asked for
 *     getch()          ERR, every time    the key that was sent
 *
 * So curses starts happily and writes its escape sequences into a pipe. What
 * it cannot do there is **report a real window size** -- 24 by 80 is
 * terminfo's default for `xterm`, not a measurement -- and **read a
 * keystroke**, because there is no keyboard at the far end of a pipe.
 *
 * The keystroke is the one that matters. It is why jlib's one curses program
 * has never been covered by anything the suite runs: #247 records that
 * jalpaca's harness lives outside the repo, where #197 had already
 * established that harnesses should not.
 *
 * It is also why `sys_pty_test` asks for a size that is *not* 24 by 80. An
 * assertion on the default passes whether or not this file does anything,
 * which is how the first version of that test passed for the wrong reason.
 *
 * ### What this matches, and it is not the screen
 *
 * `expect()` searches **the stream**, not the rendered screen. Curses writes
 * escape sequences and repaints, so what arrives is a transcript of drawing
 * commands rather than a picture. The consequences are worth knowing before
 * writing an assertion against it:
 *
 * - text that was painted and later overwritten is still in the stream, so a
 *   match does not prove the text is *on screen now*
 * - text assembled by cursor motion -- a word painted in two halves at two
 *   positions -- will not match as one string
 *
 * Interpreting the sequences well enough to answer "what does line 3 say" is
 * writing a terminal emulator, which is far more than a test harness should
 * be. So this takes the other road deliberately: assert on appearances that
 * curses writes contiguously, and accept that the question being answered is
 * "did the program ever paint this" rather than "is this visible".
 *
 * ### Four ways an assertion against this lies, all met in practice
 *
 * `app_juanaco_test` needed four attempts to assert one property -- that a
 * key added a line to the prompt rather than submitting it -- and every
 * failure was a property of the stream rather than of the program. Worth
 * reading before writing the next one:
 *
 * - **A prompt being typed cannot be matched at all.** curses repaints only
 *   the cell that changed, so text being entered arrives one character at a
 *   time at an addressed position and is never contiguous. Assert on what the
 *   program does with a *submitted* line, not on the line being typed.
 * - **Counting occurrences counts redraws, not events.** A pane repainted on
 *   every keystroke re-emits everything on it, so a count of a message read
 *   46 where the test wanted 1.
 * - **A fresh row is painted as one string; a reused row is not.** The same
 *   assertion passed early in a test and silently stopped discriminating once
 *   earlier output had filled the pane. If an assertion depends on that, it
 *   has to say so where someone reordering the test will read it.
 * - **A negative needs its own bounded wait.** `expect` returns the moment it
 *   matches, so asking `seen()` immediately afterwards reads a stream the next
 *   paint has not reached. "X never appears" can only be "X has not appeared
 *   after waiting" -- use `expect` for the negative too and spend the time on
 *   purpose.
 *
 * Each of those made a test pass against a deliberately broken build, which
 * is the only reason they were found.
 *
 * ### posix_openpt rather than forkpty
 *
 * `forkpty` is shorter and lives in libutil on glibc, which would mean a link
 * flag and a configure probe for one test. `posix_openpt` and its three
 * companions are in libc on both macOS and Linux, so this is twenty more lines
 * and no build change.
 *
 * ### What cannot be stranded
 *
 * The child gets a terminal this created; the test process keeps its own
 * stdio untouched and never enters raw mode. So the usual hazard of a
 * terminal-driving harness -- leaving the shell with no echo -- does not
 * arise here. What *can* leak is the child and the master descriptor, which
 * is what the destructor is for, and it bounds its own wait so a child that
 * ignores a hangup cannot hang the suite.
 */

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
// clock_gettime and CLOCK_MONOTONIC.  Reached transitively on macOS and not
// on glibc, where leaving this out is a build break the host cannot see --
// the #309 class, caught by the container run.
#include <ctime>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace jlib {
namespace tests {

class pty {
public:
    /**
     * Start `argv` on the slave side of a new terminal of the given size.
     *
     * @param argv argv[0] is the program; it is run directly rather than
     *        through a shell, so nothing here interprets a metacharacter
     * @param rows,cols the window size the child will read, which a curses
     *        program asks for immediately and lays itself out around
     */
    pty(const std::vector<std::string>& argv,
        unsigned short rows = 24, unsigned short cols = 80)
    {
        m_master = ::posix_openpt(O_RDWR | O_NOCTTY);

        if(m_master < 0) throw std::runtime_error("posix_openpt: " +
                                                  std::string(strerror(errno)));

        if(::grantpt(m_master) < 0 || ::unlockpt(m_master) < 0) {
            ::close(m_master);

            throw std::runtime_error("grantpt/unlockpt: " +
                                     std::string(strerror(errno)));
        }

        const char* slave = ::ptsname(m_master);

        if(!slave) {
            ::close(m_master);

            throw std::runtime_error("ptsname: " + std::string(strerror(errno)));
        }

        const std::string path = slave;

        m_child = ::fork();

        if(m_child < 0) {
            ::close(m_master);

            throw std::runtime_error("fork: " + std::string(strerror(errno)));
        }

        if(m_child == 0) {
            // **A new session first.** Without it the child keeps the test
            // runner's controlling terminal, and the whole point is that it
            // should have this one -- curses would read the wrong size and a
            // signal would go to the wrong group.
            ::setsid();

            const int fd = ::open(path.c_str(), O_RDWR);

            if(fd < 0) ::_exit(127);

#ifdef TIOCSCTTY
            ::ioctl(fd, TIOCSCTTY, 0);
#endif
            // **The size goes on the slave, here, and nowhere else.** Two
            // other places look right and are not, both measured on macOS:
            //
            //   - the master fails outright. `TIOCSWINSZ` on it returns
            //     ENOTTY, "Inappropriate ioctl for device", and the first
            //     version of this file ignored that return value, so the
            //     harness quietly set no size at all.
            //   - a slave fd the *parent* opens, sets and closes succeeds and
            //     then does not persist: reopening reports 0 by 0.
            //
            // So it has to be the descriptor the child keeps. The dup2s below
            // share this file description, so closing `fd` afterwards does
            // not drop the last reference and lose it again.
            struct winsize ws = {};

            ws.ws_row = rows;
            ws.ws_col = cols;

            if(::ioctl(fd, TIOCSWINSZ, &ws) < 0) ::_exit(125);

            ::dup2(fd, 0);
            ::dup2(fd, 1);
            ::dup2(fd, 2);

            if(fd > 2) ::close(fd);

            ::close(m_master);

            // TERM matters: curses refuses to start without a terminfo entry
            // it recognises, and the value inherited from a CI runner may be
            // "dumb" or absent. xterm is present in every terminfo database
            // this is likely to meet.
            ::setenv("TERM", "xterm", 1);

            std::vector<char*> args;

            for(const std::string& a : argv)
                args.push_back(const_cast<char*>(a.c_str()));

            args.push_back(0);

            ::execv(args[0], args.data());

            // Only reachable if execv failed, and the parent will see the
            // exit status rather than a message, because stderr is the pty.
            ::_exit(126);
        }

        // Non-blocking, so read() in the poll loop below cannot park forever
        // on a child that has gone quiet.
        const int fl = ::fcntl(m_master, F_GETFL, 0);

        ::fcntl(m_master, F_SETFL, fl | O_NONBLOCK);
    }

    ~pty() { finish(); }

    pty(const pty&) = delete;
    pty& operator=(const pty&) = delete;

    /** Type at it.  Escape is "\x1b", Return "\r" -- not "\n", which a
     *  terminal in canonical mode would not treat as submit. */
    void send(const std::string& keys) {
        const char* p = keys.data();
        std::size_t left = keys.size();

        while(left) {
            const ssize_t n = ::write(m_master, p, left);

            if(n > 0) { p += n; left -= std::size_t(n); continue; }

            if(n < 0 && (errno == EAGAIN || errno == EINTR)) continue;

            throw std::runtime_error("write to pty: " +
                                     std::string(strerror(errno)));
        }
    }

    /**
     * Read until `needle` has appeared, or the bound expires.
     *
     * Bounded rather than a sleep, and the bound is here rather than in the
     * shell because `timeout(1)` is not installed on this machine -- neither
     * is `gtimeout`. A fixed sleep would make the test slow when it passes
     * and flaky when the machine is loaded, which is the failure this suite
     * has met more than once.
     *
     * Searches everything seen so far, so an `expect()` for something that
     * arrived before the call still succeeds.
     */
    bool expect(const std::string& needle, double seconds = 5.0) {
        const double deadline = now() + seconds;

        for(;;) {
            if(m_seen.find(needle) != std::string::npos) return true;

            const double left = deadline - now();

            if(left <= 0) return false;

            if(!pump(left)) return m_seen.find(needle) != std::string::npos;
        }
    }

    /** Everything read so far: drawing commands and all. */
    const std::string& seen() const { return m_seen; }

    /**
     * Read for a while and assert nothing.
     *
     * For a caller polling a *screen* rather than waiting for a string:
     * `expect` is the wrong primitive there, because the text a test sends is
     * echoed back by the program's own prompt, so expecting it matches the
     * typing rather than the reply and returns before the program has acted.
     *
     * **A loop to the deadline, because one pump() is not a wait.** pump()
     * returns as soon as its select does, and a pty master hands back as
     * little as **one byte per read** -- measured here, 228 single-byte reads
     * in one run of `app_juanaco_test`. So a `settle(0.1)` with anything at
     * all buffered returned in microseconds having consumed one character,
     * and a poll built on it became a spin: 300 iterations of what was meant
     * to be a thirty-second wait went by in 0.1 seconds, read ~300 bytes of
     * backlog, and gave up before the program had written the line the test
     * was waiting for.
     *
     * It failed six runs out of six and **passed as soon as a printf was
     * added to pump**, which is what it looks like when the bug is that
     * nothing is waiting: the printf was the only delay in the loop.
     */
    void settle(double seconds) {
        const double deadline = now() + seconds;

        for(;;) {
            const double left = deadline - now();

            if(left <= 0) return;

            // False is the far end closed, and there is nothing further to
            // wait for -- but drain what is still buffered first, which is
            // where a program's last words live when it exits immediately
            // after writing them.
            if(!pump(left > 0.02 ? 0.02 : left)) {
                while(pump(0.0)) ;

                return;
            }
        }
    }

    /**
     * Wait for the child to exit and return its status, or -1 on timeout.
     *
     * Keeps reading while it waits, because a child that fills the pty buffer
     * and blocks on write would never reach exit if nobody drained it.
     */
    int wait_for_exit(double seconds = 5.0) {
        const double deadline = now() + seconds;

        for(;;) {
            int status = 0;
            const pid_t r = ::waitpid(m_child, &status, WNOHANG);

            if(r == m_child) { m_reaped = true; return status; }

            const double left = deadline - now();

            if(left <= 0) return -1;

            pump(left > 0.1 ? 0.1 : left);
        }
    }

private:
    static double now() {
        struct timespec ts = {};

        ::clock_gettime(CLOCK_MONOTONIC, &ts);

        return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
    }

    /** One wait-and-read.  False when the far end has closed. */
    bool pump(double seconds) {
        fd_set r;

        FD_ZERO(&r);
        FD_SET(m_master, &r);

        struct timeval tv;

        tv.tv_sec = long(seconds);
        tv.tv_usec = long((seconds - double(tv.tv_sec)) * 1e6);

        const int n = ::select(m_master + 1, &r, 0, 0, &tv);

        if(n < 0) return errno == EINTR;
        if(n == 0) return true;             // nothing yet, but still open

        char buf[4096];

        const ssize_t got = ::read(m_master, buf, sizeof(buf));

        if(got > 0) { m_seen.append(buf, std::size_t(got)); return true; }

        // EIO is how a pty master reports that the slave side closed, which
        // is the ordinary end of a child rather than an error.
        if(got == 0) return false;

        return errno == EAGAIN || errno == EINTR;
    }

    void finish() {
        if(m_master >= 0) {
            // Closing the master hangs up the child's terminal, which is how
            // a well-behaved program is asked to leave.
            ::close(m_master);

            m_master = -1;
        }

        if(m_child > 0 && !m_reaped) {
            int status = 0;

            // Bounded, then insistent: a child that ignores a hangup must not
            // be able to hang the suite.
            for(int i = 0; i < 50; i++) {
                if(::waitpid(m_child, &status, WNOHANG) == m_child) {
                    m_reaped = true;
                    break;
                }

                ::usleep(20000);
            }

            if(!m_reaped) {
                ::kill(m_child, SIGKILL);
                ::waitpid(m_child, &status, 0);
                m_reaped = true;
            }
        }
    }

    int m_master = -1;
    pid_t m_child = -1;
    bool m_reaped = false;
    std::string m_seen;
};

} // namespace tests
} // namespace jlib

#endif // JLIB_TESTS_PTY_HH
