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

#ifndef JLIB_APPS_JHTTPD_HH
#define JLIB_APPS_JHTTPD_HH

#include <jlib/net/http_server.hh>

#include <jlib/sys/sync.hh>

#include <csignal>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <future>
#include <memory>
#include <string>

/**
 * jhttpd's logging: the Combined Log Format, and the escaping it needs.
 *
 * In a header so a test can drive it without the program. The formatting is
 * the part with decisions in it; the program around it is argument parsing.
 */
namespace jhttpd {

/**
 * One field, safe to put between quotes on a line of a log file.
 *
 * **This is the whole security content of logging**, and #270 records it as
 * the first thing to look at in a jhttpd audit, so it is done here rather than
 * filed.
 *
 * A Combined line is delimited by newlines and its client-supplied fields by
 * double quotes. `method`, `target`, `referer` and `user_agent` all come from
 * the client. A log an attacker can write is worse than no log, because it is
 * believed -- and it is believed by an operator during an incident, which is
 * the worst possible moment to be reading fiction.
 *
 * ## What actually gets through, measured rather than assumed
 *
 * The obvious attack -- a newline, forging whole entries -- **cannot happen**,
 * and it is worth knowing why before trusting that. `field-value` is
 * `*field-content` over `field-vchar`, which is VCHAR or obs-text, so a raw CR
 * or LF or any other control character in a header value is refused by the
 * grammar with a 400 before anything is logged. A request-target arrives
 * percent-encoded, so it cannot carry one either. Escaping does not close that
 * hole; the parser already did.
 *
 * Two things do get through, and both are why this function exists:
 *
 *   - **A double quote.** `"` is an ordinary VCHAR and perfectly legal in a
 *     User-Agent. Unescaped, `x" 999 0 "-" "forged` produces a line whose
 *     fields parse as a different request with a different status. Nothing
 *     upstream refuses it, because there is nothing wrong with it as HTTP.
 *   - **Bytes above 0x7F**, which `obs-text` allows. They are not dangerous,
 *     but they are not necessarily valid UTF-8, and a log file that is not
 *     text is a log file that breaks whatever reads it next.
 *
 * So the live vector is field-structure forgery rather than line forgery, and
 * saying so precisely is the difference between a guard somebody keeps and one
 * somebody deletes as paranoia.
 *
 * ## The escaping is Apache's, deliberately
 *
 * Backslash escapes, `\xHH` for anything else non-printable -- which is what
 * Apache writes and therefore what log parsers already read. A format that is
 * safe and unreadable by existing tools would defeat the reason for choosing
 * Combined in the first place.
 *
 */




/**
 * How many times a reopen has been asked for.
 *
 * Set by a signal handler, so `sig_atomic_t` and nothing else: a handler may
 * not take a lock, allocate, or call `std::ofstream`.  It raises a flag and
 * returns, and the reopening happens on a thread that is allowed to do it.
 *
 * A **counter** rather than a boolean because there is more than one log. A
 * single flag would be consumed by whichever file wrote next and the other
 * would keep writing to the rotated-away inode -- which is the exact bug this
 * exists to fix, reintroduced in the fix.  Each file remembers the count it
 * last acted on.
 */
extern volatile std::sig_atomic_t reopen_requested;

/** The SIGHUP handler.  Does nothing but raise the count. */
void reopen_on_hup(int);

/**
 * A log file written by **one thread of its own**, which nothing else touches.
 *
 * ## Why not just a mutex
 *
 * It was a mutex, and that was wrong for a reason a lock cannot fix: the
 * access record is delivered on the thread that answered the request, and on
 * the async server **that is the reactor thread.** Writing there means an
 * `ofstream` append and a `flush()` -- a disk write -- on the one thread
 * carrying every connection, every timer and every handoff, on every request,
 * in a program whose whole purpose is to be left running. A lock made the
 * lines not interleave; it did not stop the disk being on the reactor.
 *
 * Nothing blocking runs on the reactor thread. See `sys::on_a_reactor_thread`.
 *
 * So `write()` hands the line to a `job_queue` with one worker and returns.
 * `post()` queues and notifies; it does not wait for room, so the caller is
 * never blocked by a slow disk.
 *
 * ## And the lock went with it
 *
 * One thread owns the stream, so nothing shares it and there is nothing to
 * guard. The reopen happens on that thread too, in the same queue, in order
 * with the writes around it -- which is simpler than the flag it replaced and
 * strictly more correct: a line can no longer be written to the old file after
 * a reopen was meant to have happened.
 *
 * The queue is unbounded, which is deliberate: the alternative is `post()`
 * waiting for room, and waiting is the thing being removed. A disk slow enough
 * for that to matter is a disk that has already lost.
 */
class logfile {
public:
    logfile() {}

    ~logfile() { close(); }

    logfile(const logfile&) = delete;
    logfile& operator=(const logfile&) = delete;

    /** Empty path means discard, which is what --access-log "" asks for. */
    bool open(const std::string& path);

    /** Queue a line.  Never blocks, never touches the file. */
    void write(const std::string& line);

    /**
     * Wait until everything queued has been written.
     *
     * For shutdown and for tests.  **Not to be called from a reactor thread**
     * -- it waits, and `job_queue::wait` refuses that anyway.
     */
    void drain();

    void close();

    bool wanted() const { return !m_path.empty(); }

private:
    /**
     * Reopen if somebody asked since the last line.
     *
     * **Called with the lock held**, from write(), which is what makes it safe
     * to touch the stream at all.
     *
     * Rotation therefore takes effect on the next line written rather than the
     * instant the signal arrives. That is the right way round: a log with
     * nothing to say does not need a new file, and doing it here means there
     * is no second thread and no window where one exists and the other does
     * not.
     */
    void reopen_if_asked();

    std::ofstream m_out;
    std::string   m_path;
    std::sig_atomic_t m_acted_on = 0;

    // One worker, so the stream has exactly one owner.  A pointer because a
    // job_queue is not movable and this is created only when a path is given.
    std::unique_ptr<jlib::sys::job_queue> m_writer;
};



inline std::string escaped(const std::string& field) {
    // A field with nothing to escape is the overwhelmingly common case, and
    // the common case should not allocate a second string to discover that.
    bool clean = true;

    for(std::size_t i = 0; i < field.size() && clean; i++) {
        const unsigned char c = static_cast<unsigned char>(field[i]);

        if(c < 0x20 || c >= 0x7F || c == '"' || c == '\\') clean = false;
    }

    if(clean) return field;

    std::string out;

    out.reserve(field.size() + 8);

    for(std::size_t i = 0; i < field.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(field[i]);

        switch(c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if(c < 0x20 || c >= 0x7F) {
                static const char* const hex = "0123456789abcdef";
                char esc[5] = { '\\', 'x', hex[c >> 4], hex[c & 0xF], 0 };

                out += esc;
            }
            else out += char(c);
        }
    }

    return out;
}

/** Combined's date: 17/Sep/2026:12:00:00 +0000, and always UTC. */
inline std::string log_date(std::time_t when) {
    static const char* const MONTH[] = { "Jan", "Feb", "Mar", "Apr", "May",
                                         "Jun", "Jul", "Aug", "Sep", "Oct",
                                         "Nov", "Dec" };
    std::tm tm;

    // UTC rather than local time.  A log that changes meaning twice a year is
    // one whose timestamps cannot be compared across the change, and the hour
    // that repeats in autumn is the one an incident will land in.
    ::gmtime_r(&when, &tm);

    char buf[64];

    std::snprintf(buf, sizeof buf, "%02d/%s/%04d:%02d:%02d:%02d +0000",
                  tm.tm_mday, MONTH[tm.tm_mon], tm.tm_year + 1900,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);

    return buf;
}

inline std::string combined(const jlib::net::http::server::access& a,
                     std::time_t when)
{
    std::ostringstream o;

    // `-` for anything absent, which is the format's convention and is why a
    // field is never simply empty: an empty quoted field and a missing one
    // read differently to every tool that parses these.
    const std::string user = a.user.empty() ? "-" : escaped(a.user);
    const std::string referer = a.referer.empty() ? "-" : escaped(a.referer);
    const std::string agent = a.user_agent.empty() ? "-" : escaped(a.user_agent);

    // The request line as three escaped pieces rather than one: a client that
    // sends a target with a space in it would otherwise produce a line whose
    // fields do not line up, and the space is not escapable without inventing
    // a format.
    std::string request;

    if(a.method.empty() && a.target.empty()) request = "-";
    else {
        request = escaped(a.method) + " " + escaped(a.target);

        if(!a.version.empty()) request += " " + escaped(a.version);
    }

    o << (a.peer.empty() ? "-" : a.peer)
      << " - "                       // identd, which nothing has run since 1995
      << user
      << " [" << log_date(when) << "] "
      << "\"" << request << "\" "
      << a.status << " "
      << a.bytes
      << " \"" << referer << "\""
      << " \"" << agent << "\"";

    return o.str();
}

inline volatile std::sig_atomic_t reopen_requested = 0;

inline void reopen_on_hup(int) { reopen_requested++; }

inline bool logfile::open(const std::string& path) {
    if(path.empty()) return true;

    m_path = path;
    m_acted_on = reopen_requested;

    m_out.open(path.c_str(), std::ios::out | std::ios::app);

    if(!m_out.is_open()) { m_path.clear(); return false; }

    // One worker, started by the constructor.
    m_writer.reset(new jlib::sys::job_queue(1));

    return true;
}

inline void logfile::drain() {
    if(!m_writer) return;

    // **A barrier, not an empty queue.**  job_queue::size() is the *depth*, and
    // a job a worker has taken is running rather than queued -- so waiting for
    // depth zero returns while the line is still on its way to the disk. That
    // is documented on max_queued and I wrote this the wrong way anyway; the
    // rotation tests caught it.
    //
    // One worker and FIFO order, so a job posted now runs after everything
    // posted before it. Waiting for *that* job is waiting for all of them.
    std::promise<void> done;
    std::future<void> wait = done.get_future();

    m_writer->post([&done] { done.set_value(); });

    wait.wait();
}

inline void logfile::close() {
    if(!m_writer) return;

    // Drained before stopping, so a line queued a moment before shutdown is
    // written rather than dropped.  stop(true) would drain the queue too, but
    // saying it here makes the order explicit.
    drain();

    m_writer->stop(true);
    m_writer->join();
    m_writer.reset();

    m_out.close();
}

/** **Writer thread only**, which is what makes it safe to touch the stream. */
inline void logfile::reopen_if_asked() {
    const std::sig_atomic_t asked = reopen_requested;

    if(asked == m_acted_on || m_path.empty()) return;

    m_acted_on = asked;

    m_out.close();
    m_out.clear();

    // Appending, because the point of rotation is that the *old* file was
    // moved away: this creates a new one, and if somebody instead truncated
    // the file in place, appending is still what preserves what is there.
    m_out.open(m_path.c_str(), std::ios::out | std::ios::app);
}

inline void logfile::write(const std::string& line) {
    if(m_path.empty() || !m_writer) return;

    // **This is the whole of what the caller pays**: a copy and a queue push.
    // Everything below the lambda happens on the writer's thread.
    m_writer->post([this, line] {
        reopen_if_asked();

        if(!m_out.is_open()) return;

        m_out << line << "\n";
        m_out.flush();
    });
}

}

#endif
