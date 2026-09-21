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
#include <jlib/sys/sys.hh>
#include <jlib/util/conf.hh>

#include <atomic>
#include <functional>
#include <thread>

#ifdef HAVE_PWHASH
#include <sodium.h>
#endif

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <csignal>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <future>
#include <map>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

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
 * A credential file: one `user:hash` per line, Argon2id.
 *
 * ## Why Argon2id and not htpasswd
 *
 * libsodium is already a dependency -- `jlib/crypt` links it -- and
 * `crypto_pwhash_str` writes a self-describing, salted, memory-hard hash that
 * `crypto_pwhash_str_verify` checks in **constant time**:
 *
 *     root:$argon2id$v=19$m=65536,t=2,p=1$<salt>$<hash>
 *
 * That last property is not a nicety. The auth work in #252 recorded "nothing
 * here is constant-time; a `==` on a password is a timing oracle" as a known
 * gap, and this closes it by construction rather than by care -- there is no
 * comparison in jhttpd to get wrong.
 *
 * Real `htpasswd` files are **not** read. htpasswd writes bcrypt by default,
 * libsodium has no bcrypt, and implementing one to be compatible with a format
 * that is weaker than what is already available is the wrong trade. Apache
 * compatibility is a #239 stretch goal and belongs with the config work, not
 * with the first credential this server ever checks.
 *
 * ## The file
 *
 * Comments and blank lines are skipped, so a file can say who it is for.
 * A line without a colon, or with an empty user, is an error rather than a
 * skipped line: a credential file with a typo in it should stop the server,
 * not silently guard less than the operator thinks.
 *
 * **Permissions are checked.** A file readable by anyone but its owner is
 * refused. Argon2id makes the hashes expensive to attack rather than
 * impossible, and a world-readable list of them is an offline attack somebody
 * has been handed.
 */
class credentials {
public:
    /** @throws std::runtime_error with the line number, if it cannot be read. */
    void load(const std::string& path);

    /**
     * Is this the password for this user?
     *
     * False for an unknown user, and the work is done anyway -- see the note
     * at the implementation about why an early return would be a different
     * kind of leak.
     */
    bool check(const std::string& user, const std::string& password) const;

    std::size_t size() const {
        const std::shared_ptr<const table> t = snapshot();

        return t ? t->hashes.size() : 0;
    }

private:
    /**
     * The hashes and the decoy, as one immutable object.
     *
     * **Replaced wholesale, never edited.**  load() can be called again while
     * requests are being checked -- that is what makes SIGHUP able to pick up
     * a new password file -- and check() holds a reference into this map
     * across a deliberately slow Argon2id verification. Editing the map under
     * it would invalidate that reference mid-verify; swapping the pointer
     * cannot, because the reader's shared_ptr keeps the old table alive for
     * exactly as long as it is still reading.
     *
     * The decoy belongs in here with them: it is generated per load, and a
     * reader that got the new hashes and the old decoy would be checking an
     * unknown user against a hash from a file that is no longer in use. It
     * would still take the right amount of time, which is the only thing the
     * decoy is for, but one object with one lifetime is easier to be sure of.
     */
    struct table {
        std::map<std::string, std::string> hashes;

        // What an unknown user is checked against.  See check().
        std::string decoy;
    };

    /**
     * Guarded by a mutex, which is a choice rather than a constraint.
     *
     * Both platforms build as C++20. Two ways to do this without a lock, and
     * neither was ruled out by the language level:
     *
     * - `std::atomic<std::shared_ptr<T>>`, the C++20 specialisation, is
     *   **absent from the libc++ shipped with Xcode** -- libstdc++ defines
     *   `__cpp_lib_atomic_shared_ptr` and libc++ does not, so there it falls
     *   through to the primary template and fails an `is_trivially_copyable`
     *   assertion. That one would need the macro and an #else.
     *
     * - `std::atomic_load` / `std::atomic_store` on a plain shared_ptr, the
     *   C++11 spelling, compile and run on both with no warning. They are
     *   deprecated in favour of the specialisation above, which is the only
     *   thing against them.
     *
     * The second would work here and is a reasonable thing to prefer; a
     * sibling codebase uses exactly it for exactly this shape. The mutex wins
     * on the numbers rather than on principle: it is held for a refcount bump
     * and released, while the Argon2id verification that follows -- the
     * expensive part, and the whole reason the hash is worth anything -- takes
     * milliseconds. A lock that is a rounding error next to the work it
     * guards is not worth a deprecated API to avoid.
     *
     * What would be worth avoiding is holding it *across* the verify, which
     * would serialise every authentication in the server. That is why the
     * snapshot is taken and the lock dropped before check() does any work.
     */
    mutable std::mutex                 m_lock;
    std::shared_ptr<const table>       m_table;

    std::shared_ptr<const table> snapshot() const {
        std::lock_guard<std::mutex> hold(m_lock);

        return m_table;
    }
};

/**
 * Hash a password the way `credentials` expects to read one.
 *
 * Exposed because the alternative to `jhttpd --hash-password` is telling
 * somebody to produce an Argon2id string elsewhere, and the way that ends is a
 * `$apr1$` line in a file that only understands `$argon2id$`.
 *
 * @throws std::runtime_error if libsodium is not available or the hash fails
 */
std::string hash_password(const std::string& password);

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

/**
 * Month and day names in the C locale, shared by both log formats.
 *
 * Spelled out rather than taken from `strftime`, because `%b` and `%a` follow
 * the locale: a server started under a different `LANG` would write a log
 * whose dates no existing parser reads, and the failure would be invisible
 * until something tried to read it.
 */
inline constexpr const char* const MONTH_NAME[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

inline constexpr const char* const DAY_NAME[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

/** Combined's date: 17/Sep/2026:12:00:00 +0000, and always UTC. */
inline std::string log_date(std::time_t when) {
    const char* const* MONTH = MONTH_NAME;

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

/**
 * Which access-log format to write.
 *
 * Both are Apache's, spelled exactly as Apache spells them, because the point
 * of either is that existing tools already read it:
 *
 *     combined        %h %l %u %t "%r" %>s %O "%{Referer}i" "%{User-Agent}i"
 *     vhost_combined  %v:%p %h %l %u %t "%r" %>s %O "%{Referer}i" ...
 *
 * `combined` stays the default. Changing the shape of a log that is already
 * being parsed is a change to somebody's tooling, and #315 is a question an
 * operator asks occasionally -- not one worth breaking every reader for.
 */
enum class log_shape { combined, vhost_combined };

inline bool log_shape_named(const std::string& name, log_shape& out) {
    if(name == "combined")       { out = log_shape::combined; return true; }
    if(name == "vhost_combined") { out = log_shape::vhost_combined; return true; }

    return false;
}

inline std::string combined(const jlib::net::http::server::access& a,
                     std::time_t when,
                     log_shape shape = log_shape::combined)
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

    // **`%v:%p`**, and the port from the listener rather than the Host header.
    //
    // A client may put a port in Host, or the wrong one, or none; the listener
    // knows which socket accepted the connection and cannot be lied to. That
    // is the whole value of the field -- "is anything still using http?" is
    // only answerable if the answer does not come from the client.
    if(shape == log_shape::vhost_combined) {
        o << (a.host.empty() ? "-" : escaped(a.host))
          << ":" << a.local_port << " ";
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

/**
 * Apache's error-log levels, with Apache's names and in Apache's order.
 *
 * **Most urgent first**, so "write everything at or above `notice`" is a `<=`
 * on the enum. Both `LogLevel` and nginx's `error_log` level argument work
 * that way round, and a comparison written the other way silently logs either
 * nothing or all of it -- neither of which announces itself.
 */
enum class level { emerg, alert, crit, error, warn, notice, info, debug };

inline const char* level_name(level l) {
    switch(l) {
        case level::emerg:  return "emerg";
        case level::alert:  return "alert";
        case level::crit:   return "crit";
        case level::error:  return "error";
        case level::warn:   return "warn";
        case level::notice: return "notice";
        case level::info:   return "info";
        case level::debug:  return "debug";
    }

    return "error";
}

/**
 * The level of that name, or false.
 *
 * False rather than a default, so `error_log /var/log/jhttpd/error.log notic;`
 * is refused at startup instead of quietly meaning something else. A typo in a
 * level is not detectable later: the log simply has less in it than expected,
 * which reads like a quiet day.
 */
inline bool level_named(const std::string& name, level& out) {
    static const struct { const char* name; level l; } KNOWN[] = {
        { "emerg",  level::emerg  }, { "alert", level::alert },
        { "crit",   level::crit   }, { "error", level::error },
        { "warn",   level::warn   }, { "notice", level::notice },
        { "info",   level::info   }, { "debug", level::debug  }
    };

    for(std::size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++) {
        if(name == KNOWN[i].name) {
            out = KNOWN[i].l;

            return true;
        }
    }

    return false;
}

/**
 * The error log's date: `Sat Sep 20 16:03:22.487000 2026`.
 *
 * Apache's `ErrorLogFormat` default, down to the microseconds, because the
 * point of matching it is that existing tooling reads it.
 *
 * **UTC, for the same reason the access log is** -- a log that changes meaning
 * twice a year cannot have its timestamps compared across the change, and the
 * hour that repeats in autumn is the one an incident lands in. Apache writes
 * local time here and no zone at all, which is a real weakness of the format;
 * a server whose clock is UTC, as a server's should be, is indistinguishable.
 */
inline std::string error_date(std::time_t sec, long usec) {
    std::tm tm;

    ::gmtime_r(&sec, &tm);

    char buf[64];

    std::snprintf(buf, sizeof buf, "%s %s %02d %02d:%02d:%02d.%06ld %04d",
                  DAY_NAME[tm.tm_wday], MONTH_NAME[tm.tm_mon], tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, usec, tm.tm_year + 1900);

    return buf;
}

/** Now, to the microsecond, for an error line. */
inline void error_now(std::time_t& sec, long& usec) {
    struct timeval tv;

    ::gettimeofday(&tv, 0);

    sec = tv.tv_sec;
    usec = tv.tv_usec;
}

/**
 * One error line, in the shape Apache's default `ErrorLogFormat` produces:
 *
 *     [Sat Sep 20 16:03:22.487000 2026] [http:error] [pid 619:tid 4310] \
 *         [client 45.43.62.37:51834] an encoded path separator in the target
 *
 * `module` is this server's equivalent of Apache's -- `core` for lifecycle,
 * `http` for a request, `ssl` for a handshake. It is not a module system; it
 * is the field an operator greps, and having it empty would waste the shape.
 *
 * **No `AH#####` code.** Apache's exist for a message registry, and minting
 * our own would commit us to numbering every message forever for greppability
 * that stable message text already gives.
 *
 * The client field is omitted entirely when there is no client, which is what
 * Apache does for its lifecycle notices -- an empty `[client ]` would be a
 * field every parser has to special-case.
 */
inline std::string error_line(level l, const char* module,
                              const std::string& client,
                              const std::string& message,
                              std::time_t sec, long usec)
{
    std::ostringstream o;

    o << "[" << error_date(sec, usec) << "] "
      << "[" << module << ":" << level_name(l) << "] "
      << "[pid " << static_cast<long>(::getpid())
      << ":tid " << static_cast<unsigned long>(
             std::hash<std::thread::id>()(std::this_thread::get_id()))
      << "]";

    if(!client.empty()) o << " [client " << client << "]";

    // Escaped for the same reason the access log is: an error message quotes
    // the value that caused it, and that value is one the client chose. A log
    // an attacker can write a newline into is worse than no log.
    o << " " << escaped(message);

    return o.str();
}

/**
 * The error log: a file, a level, and Apache's line shape.
 *
 * The level is the whole point of this type. #322 measured 540 error lines in
 * a day on six sites, of which **none was something an operator could act
 * on** -- failed handshakes from scanners, peers that connected and went away.
 * A log that is 98% noise is one nobody reads, which costs more than the noise
 * does: the four lines that mattered were in there too.
 *
 * So nothing is deleted; it is levelled. Handshake failures and peers that say
 * nothing are `info`, below the `notice` default, and `error_log <path> info;`
 * brings them back for the afternoon somebody is debugging a client.
 */
class error_log {
public:
    /**
     * `-` means stderr, which is what a foreground run wants -- and what the
     * systemd unit relies on to put lines in the journal.
     */
    bool open(const std::string& path, level at) {
        level_now(at);
        m_stderr = path == "-";

        if(m_stderr) return true;

        return m_file.open(path);
    }

    /**
     * Whether a line at this level would be written.
     *
     * Exposed so a caller can skip *building* a message it is about to throw
     * away. The noisy paths are the ones that would pay for it.
     */
    bool says(level l) const {
        return l <= m_at.load(std::memory_order_relaxed);
    }

    /**
     * Change the level on a running server (#324).
     *
     * **Relaxed, and that is sufficient rather than merely cheap.** No
     * invariant ties the level to any other state, so the only consequence of
     * a stale read is that a line either side of the change is sorted by the
     * old value -- which is already true of any implementation, because a line
     * in flight was sorted before the change arrived.
     *
     * A mutex would have been wrong here for a reason the rest of this type
     * already answers: `says()` is called on whichever thread answered the
     * request, and on the async server that is the reactor thread. Taking a
     * lock per log line on the reactor is the thing `logfile` exists to avoid.
     */
    void level_now(level at) { m_at.store(at, std::memory_order_relaxed); }

    level at() const { return m_at.load(std::memory_order_relaxed); }

    void write(level l, const char* module, const std::string& client,
               const std::string& message)
    {
        if(!says(l)) return;

        std::time_t sec = 0;
        long        usec = 0;

        error_now(sec, usec);

        const std::string line = error_line(l, module, client, message,
                                            sec, usec);

        if(m_stderr) std::cerr << line << "\n";
        else         m_file.write(line);
    }

    void drain() { if(!m_stderr) m_file.drain(); }

    void close() { if(!m_stderr) m_file.close(); }

private:
    logfile m_file;
    bool    m_stderr = true;

    // Atomic because a reload changes it from the reload thread while the
    // reactor thread is reading it on every request.  See level_now().
    std::atomic<level> m_at{level::notice};
};

/**
 * A refusal's text as one line.
 *
 * The same string is a response body, where it ends in a newline because a
 * body should, and a log message, where a newline would end the line early --
 * or be escaped into a visible `\n` that reads like a mistake.
 */
inline std::string one_line(const std::string& s) {
    std::string out = s;

    while(!out.empty() && (out[out.size() - 1] == '\n' ||
                           out[out.size() - 1] == '\r' ||
                           out[out.size() - 1] == ' '))
        out.erase(out.size() - 1);

    return out;
}

/** Apache's `[client 1.2.3.4:51834]`, or empty when there is no peer. */
inline std::string client_of(const jlib::sys::peer& p) {
    if(p.address.empty()) return "";

    return p.address + ":" + std::to_string(p.port);
}

/** Which subsystem a failure belongs to, and how much it matters. */
struct sorted_error {
    const char* module;
    level       at;
};

/**
 * Sort an exception into a module and a level.
 *
 * **The levels here are the whole of #322.** Measured over a day on six live
 * sites, 540 error lines: 351 failed handshakes, 151 peers that connected and
 * said nothing or stopped mid-head, and a handful that meant something. At
 * `error` the log was unreadable and therefore unread.
 *
 * Nothing is dropped. A failed handshake is still a fact, and
 * `error_log <path> info;` still writes it. It is simply not an *error*: the
 * peer offered ciphers we do not have, spoke a protocol we do not, or went
 * away -- none of which is a thing an operator does anything about, and all of
 * which Apache leaves below its default level too.
 *
 * Matched on message text rather than exception type because the type does not
 * carry the distinction: a handshake failure and a malformed request line are
 * both thrown as the same class, and the difference that matters -- whose
 * fault it is -- lives only in the message.
 */
inline sorted_error sort_error(const std::exception& e) {
    const std::string what = e.what();

    // The peer's problem, in every case: what it offered, what it spoke, or
    // that it left. 351 of the 540.
    if(what.find("SSL_accept failed") != std::string::npos)
        return { "ssl", level::info };

    // Connected and said nothing, or stopped part-way through. Already
    // answered with a 408 in the access log, which is where Apache puts it and
    // where it can be counted.
    if(what.find("closed before the message head ended") != std::string::npos ||
       what.find("octets short of the body it promised") != std::string::npos)
        return { "http", level::info };

    // **The peer hung up while we were answering.**
    //
    // Thrown by async_writer as a plain runtime_error, so it used to reach the
    // fallback below and be logged `core:error` -- the bucket that means "a
    // server's own failure", and the one thing a level must never hide. Seen
    // in production as
    //
    //     [core:error] [client 47.250.55.210:14176] the peer closed while 160
    //     octets were still to be written
    //
    // which reads as the server breaking while writing. It is a client that
    // closed a tab, and it belongs with the other went-away cases.
    if(what.find("octets were still to be written") != std::string::npos)
        return { "http", level::info };

    // **An empty request head is a knock, not a diagnosis.**
    //
    // A peer that sends a bare CRLF and stops has told us nothing an operator
    // can act on, and the access log already records the 400 with the address
    // -- which is the whole of what happened. Apache writes nothing here.
    //
    // Measured: 21 of 50 error lines in eleven hours were this, all from one
    // /24 scanning in a two-minute burst. #322 is about exactly that ratio.
    if(what.find("the request head is empty") != std::string::npos)
        return { "http", level::info };

    // A request we could read enough of to refuse: a bad request line, a
    // header section that made no sense. This *is* evidence -- it is what
    // Apache logs as AH00126 at error -- so it stays at error.
    if(dynamic_cast<const jlib::util::http::error*>(&e))
        return { "http", level::error };

    // Anything else is ours until proven otherwise, and a server's own failure
    // is the one thing that must never be filtered out by a level.
    return { "core", level::error };
}

inline std::string hash_password(const std::string& password) {
#ifdef HAVE_PWHASH
    if(::sodium_init() < 0)
        throw std::runtime_error("libsodium would not start");

    char out[crypto_pwhash_STRBYTES];

    // INTERACTIVE rather than MODERATE: this is checked on a request, and a
    // parameter set that takes a second to verify is a denial of service
    // anybody can aim at the server by guessing wrong repeatedly.  The rate
    // limiter bounds how often that happens; the cost per attempt should still
    // be a cost the server can afford.
    if(::crypto_pwhash_str(out, password.data(), password.size(),
                           crypto_pwhash_OPSLIMIT_INTERACTIVE,
                           crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
    {
        throw std::runtime_error("could not hash the password (out of memory?)");
    }

    return std::string(out);
#else
    (void)password;

    throw std::runtime_error("this jhttpd was built without libsodium, so it "
                             "cannot hash or check a password");
#endif
}

inline void credentials::load(const std::string& path) {
#ifndef HAVE_PWHASH
    (void)path;

    throw std::runtime_error("this jhttpd was built without libsodium, so it "
                             "cannot check a password; --protect is refused "
                             "rather than ignored");
#else
    struct stat st;

    if(::stat(path.c_str(), &st) != 0)
        throw std::runtime_error("cannot read \"" + path + "\"");

    // **Refused rather than warned about.**  Argon2id makes these expensive to
    // attack, not impossible, and a world-readable list of them is an offline
    // attack handed to whoever can read the disk.  A warning would be ignored
    // by exactly the deployment that needs it.
    if(st.st_mode & (S_IRWXG | S_IRWXO)) {
        throw std::runtime_error("\"" + path + "\" is readable by more than "
                                 "its owner; chmod 600 it");
    }

    std::ifstream in(path.c_str());

    if(!in) throw std::runtime_error("cannot open \"" + path + "\"");

    // Built to the side, and published at the end.  A partly-filled table is
    // never visible to a request: either the whole file was read or the old
    // one is still in use, which is what lets a reload that finds a malformed
    // line leave the server working.
    std::shared_ptr<table> built(new table);

    std::string line;
    int no = 0;

    while(std::getline(in, line)) {
        no++;

        while(!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.erase(line.size() - 1);

        if(line.empty() || line[0] == '#') continue;

        const std::string::size_type colon = line.find(':');

        // An error rather than a skipped line: a credential file with a typo
        // in it should stop the server rather than quietly guard less than the
        // operator believes it does.
        if(colon == std::string::npos || colon == 0) {
            throw std::runtime_error("\"" + path + "\" line " +
                                     std::to_string(no) +
                                     ": expected user:hash");
        }

        built->hashes[line.substr(0, colon)] = line.substr(colon + 1);
    }

    if(built->hashes.empty())
        throw std::runtime_error("\"" + path + "\" has no credentials in it");

    // The decoy, at the same parameters as everything else, over a password
    // nobody can send: 32 random bytes rather than a word somebody might.
    unsigned char noise[32];

    ::randombytes_buf(noise, sizeof noise);

    built->decoy = hash_password(std::string(reinterpret_cast<char*>(noise),
                                        sizeof noise));

    // **The last thing.**  Every throw above this line leaves the previous
    // table in place, so a reload that finds a missing file, a world-readable
    // one, or a malformed line changes nothing and the server keeps checking
    // against what it had.
    {
        std::lock_guard<std::mutex> hold(m_lock);

        m_table = built;
    }
#endif
}

inline bool credentials::check(const std::string& user,
                               const std::string& password) const
{
#ifndef HAVE_PWHASH
    (void)user; (void)password;

    return false;
#else
    // One load, held for the whole check: the table this names stays alive
    // until `t` goes, so a reload during the verify below cannot pull the
    // hash out from under it.
    const std::shared_ptr<const table> t = snapshot();

    if(!t) return false;

    const std::map<std::string, std::string>::const_iterator i =
        t->hashes.find(user);

    // **An unknown user still pays for a verification.**
    //
    // Returning early makes "no such user" faster than "wrong password", and
    // the difference is measurable from outside -- Argon2id is deliberately
    // slow, so the gap is milliseconds rather than nanoseconds. That turns the
    // credential file into a user-enumeration oracle, which is worth more to an
    // attacker than it sounds: it is the half of a guess that does not change.
    //
    // So an unknown user is checked against a decoy, which fails, and takes
    // the time a real failure takes.
    //
    // **Generated at load, not written here as a literal.**  A hand-written
    // hash that is not a valid encoding is rejected by the parser in
    // microseconds, which would make the decoy *faster* than a real check and
    // hand back exactly the signal it exists to hide -- and it would look
    // correct while doing it.
    const std::string& against = i == t->hashes.end() ? t->decoy : i->second;

    const int ok = ::crypto_pwhash_str_verify(against.c_str(), password.data(),
                                              password.size());

    // The verification itself is constant-time; this is only about not
    // letting the *lookup* leak.
    return i != t->hashes.end() && ok == 0;
#endif
}

/** A path that needs a credential, and where the credentials are. */
struct guard {
    std::string prefix;
    std::string realm;
    std::string file;
};

/** A name with a root of its own, and optionally a certificate of its own. */
struct site {
    std::string name;
    std::string root;
    std::string cert;
    std::string key;
};

/**
 * One port to bind, and what it does.
 *
 * `ssl` is nginx's `listen 443 ssl;`, which used to be parsed and thrown away
 * because a single-port server took its TLS from having a certificate at all.
 * With several ports it is the only way to say *which* of them is encrypted,
 * so it now decides.
 *
 * `redirect` is `listen 80 redirect;` -- answer 301 and point at the TLS port,
 * which is what a plaintext port mostly exists for. Per listener rather than a
 * server-wide switch, so a plaintext port that genuinely serves can sit beside
 * one that bounces.
 */
struct listen_spec {
    std::string    host = "127.0.0.1";
    unsigned short port = 8080;
    bool           ssl = false;
    bool           redirect = false;
    std::size_t    line = 0;
};

struct options {
    /**
     * Empty means the default one, filled in after the walk.
     *
     * Not defaulted to a single entry here: "the operator said nothing" and
     * "the operator asked for 8080" have to be told apart, or a --port flag
     * could not replace a list it was never sure existed.
     */
    std::vector<listen_spec> listens;

    std::string    root = ".";
    std::string    prefix = "/";
    std::string    access_log = "-";
    std::string    error_log = "-";

    /**
     * The level at or above which an error line is written.
     *
     * `notice` rather than `error`, because the lifecycle lines this default
     * exists to keep -- started, reloaded, shutting down -- are notices, and
     * they are the ones that let a log be dated against a deploy.
     */
    level          error_level = level::notice;

    /** Combined, unless the config asks for vhost_combined.  See #315. */
    log_shape      log_format = log_shape::combined;
    std::string    cache_control;

    /**
     * What a directory is answered with, in order.
     *
     * nginx spells it `index`; Apache spells it `DirectoryIndex` and enables
     * it globally in mods-enabled/dir.conf, which is why a vhost never
     * mentions it. `index;` with no arguments turns it off, and then a
     * directory is a 404.
     */
    std::vector<std::string> index = { "index.html" };
    unsigned int   threads = 4;
    bool           async = false;

    std::string    cert;
    std::string    key;

    // Zero is off for both, which is what the library defaults to.  jhttpd
    // does not pick a limit on the operator's behalf: a rate that is wrong is
    // worse than none, because it is wrong in a way that looks deliberate.
    double         rate = 0;
    double         burst = 0;
    std::size_t    max_per_address = 0;

    // These do have defaults, because they are the library's and a server
    // that quietly widened them would be removing a bound somebody is
    // relying on.  Mirrored here so --help can print them.
    std::size_t    max_connections = 256;
    double         io_timeout = 30;
    double         initial_idle_timeout = 5;
    double         idle_timeout = 60;
    std::size_t    max_requests = 100;

    // In the order they were given.  A list rather than one, because a server
    // with a private area usually has more than one of them.
    //
    // **Split once, at the point of entry, rather than carried as text.**
    // These used to be the raw `PREFIX:REALM:FILE` and `NAME:ROOT:CERT:KEY`
    // strings, re-split at each of three use sites.  A config file can say
    // these without colons at all, and packing its fields back into a string
    // so they could be taken apart again would re-introduce exactly the
    // ambiguity the file exists to remove -- and would do it to values that
    // never had it.
    std::vector<guard> protect;
    std::vector<site>  vhosts;

    bool           hash_password = false;

    /** --test: check the config and what it names, then exit. */
    bool           test = false;

    // Read before any flag is applied, so a flag overrides what it says.
    std::string    config;

    std::string    user;
    std::string    group;
    std::string    pidfile;
    bool           allow_root = false;
    bool           daemon = false;

    /**
     * `--forking`: the unit says it is Type=forking, so `daemon;` is required.
     *
     * Set by the flag and never by the config, because it is a fact about how
     * this process will be supervised rather than about what it should serve.
     * See the check in --test (#334).
     */
    bool           forking = false;
};


// -------------------------------------------------------------- the config

/**
 * Applying a parsed config to `options`.
 *
 * ## Which names
 *
 * nginx's own, **wherever the meaning is genuinely the same** -- `listen`,
 * `root`, `server_name`, `ssl_certificate`, `ssl_certificate_key`,
 * `access_log`, `error_log`, `user`, `daemon`, `pid`, `location`,
 * `auth_basic`, `auth_basic_user_file`, `keepalive_timeout`,
 * `keepalive_requests`, `client_header_timeout`. An operator who knows nginx
 * should be able to guess these and be right.
 *
 * Where jhttpd's setting is *not* nginx's, it gets a name of its own rather
 * than a borrowed one: `prefix`, `cache_control`, `threads`, `async`,
 * `allow_root`, `max_connections`, `max_per_address`, `request_rate`,
 * `request_burst`, `io_timeout`. The temptation was `limit_rate` and
 * `limit_conn`, and both were rejected: nginx's `limit_rate` is *bandwidth*,
 * not request rate, and its `limit_conn` is per-key rather than a total. A
 * familiar name that means something else is worse than an unfamiliar one,
 * because it is wrong in a way the operator has no reason to check.
 *
 * ## Which file wins
 *
 * The config is read first and every flag is applied over it, so **a flag
 * always overrides the file**. That is one rule with no exceptions rather than
 * a per-setting story, and it is the one people expect from a daemon.
 */

[[noreturn]] inline void wrong(const jlib::util::conf::directive& d, const std::string& why) {
    throw jlib::util::conf::error("\"" + d.name + "\" " + why, d.line);
}

/**
 * **The arity check, which is what catches a forgotten semicolon.**
 *
 * util::conf cannot: a newline is whitespace there, so `listen 8080` with no
 * `;` followed by `root /srv;` parses as one `listen` holding three arguments.
 * That is nginx's behaviour too, and it has to be, or a directive could not
 * span lines. So the parser hands up something well-formed and wrong, and the
 * only thing standing between that and a server listening on a port nobody
 * chose is counting the arguments here.
 *
 * Which means every directive below must call this, including the ones that
 * take none -- `daemon 8080 root /srv;` is what a missing semicolon after
 * `daemon` looks like.
 */
inline void arity(const jlib::util::conf::directive& d, std::size_t least, std::size_t most) {
    if(d.args.size() >= least && d.args.size() <= most) return;

    std::string want = std::to_string(least);

    if(most != least) {
        want += most == std::size_t(-1) ? " or more"
                                        : " to " + std::to_string(most);
    }

    wrong(d, "takes " + want + " argument" + (most == 1 ? "" : "s") +
             ", got " + std::to_string(d.args.size()) +
             (d.args.size() > least
                  ? " -- a missing \";\" on the line before reads like this"
                  : ""));
}

/** A directive that must not have a block. */
inline void plain(const jlib::util::conf::directive& d, std::size_t least, std::size_t most) {
    if(d.blocked) wrong(d, "does not take a { } block");

    arity(d, least, most);
}

/** A directive that must have one. */
inline void blocked(const jlib::util::conf::directive& d, std::size_t least,
             std::size_t most) {
    if(!d.blocked) wrong(d, "wants a { } block");

    arity(d, least, most);
}

inline double as_number(const jlib::util::conf::directive& d, std::size_t i) {
    const std::string& s = d.arg(i);
    char*              end = 0;
    const double       v = std::strtod(s.c_str(), &end);

    // strtod reports failure by not moving `end`, which is the only way to
    // tell "0" from a word -- std::atof answers 0 for both, and a rate of zero
    // means "no limit", so a typo would silently turn a limit off.
    if(end == s.c_str() || *end != '\0' || v < 0)
        wrong(d, "wants a number, got \"" + s + "\"");

    return v;
}

inline std::size_t as_count(const jlib::util::conf::directive& d, std::size_t i) {
    return std::size_t(as_number(d, i));
}

/** `on` or `off`, as nginx spells a boolean.  Absent means on. */
inline bool as_flag(const jlib::util::conf::directive& d) {
    if(d.args.empty()) return true;
    if(d.arg(0) == "on") return true;
    if(d.arg(0) == "off") return false;

    wrong(d, "wants \"on\" or \"off\", got \"" + d.arg(0) + "\"");
}

/**
 * `PORT`, or `ADDR:PORT`.
 *
 * The last colon that is not inside brackets separates them, so `[::1]:8080`
 * works and a bare `::1` is not mistaken for a port.
 */
inline void as_listen(const jlib::util::conf::directive& d, options& o) {
    const std::string& s = d.arg(0);
    const std::string::size_type close = s.rfind(']');
    const std::string::size_type colon =
        s.rfind(':') == std::string::npos || (close != std::string::npos &&
                                              s.rfind(':') < close)
            ? std::string::npos
            : s.rfind(':');

    const std::string port = colon == std::string::npos ? s : s.substr(colon + 1);

    listen_spec bind;

    bind.line = d.line;

    // A host only when one was given.  This used to assign into a shared
    // `o.host`, so `listen 1.2.3.4:443;` followed by `listen 8080;` left the
    // second listening on 1.2.3.4 -- the assignment was conditional and the
    // field was not per-listener.  A struct per directive is what fixes it.
    if(colon != std::string::npos) bind.host = s.substr(0, colon);

    for(std::size_t i = 0; i < port.size(); i++) {
        if(port[i] < '0' || port[i] > '9') wrong(d, "wants a port, got \"" + s + "\"");
    }

    if(port.empty()) wrong(d, "wants a port, got \"" + s + "\"");

    bind.port = (unsigned short)std::atoi(port.c_str());

    for(std::size_t i = 1; i < d.args.size(); i++) {
        if(d.arg(i) == "ssl") bind.ssl = true;
        else if(d.arg(i) == "redirect") bind.redirect = true;
        else wrong(d, "does not know \"" + d.arg(i) + "\"");
    }

    // No check that ssl and redirect are not both given: they are opposites,
    // but `listen` takes at most two arguments, so saying both needs three and
    // the arity check has already refused it.  A guard here would be a branch
    // that cannot run.

    for(std::size_t i = 0; i < o.listens.size(); i++) {
        if(o.listens[i].port == bind.port && o.listens[i].host == bind.host)
            wrong(d, "binds " + s + " twice");
    }

    o.listens.push_back(bind);
}

inline void apply_location(const jlib::util::conf::directive& d, options& o) {
    blocked(d, 1, 1);

    guard g;

    g.prefix = d.arg(0);

    for(const jlib::util::conf::directive& e : d.block) {
        if(e.name == "auth_basic") { plain(e, 1, 1); g.realm = e.arg(0); }
        else if(e.name == "auth_basic_user_file") { plain(e, 1, 1); g.file = e.arg(0); }
        else throw jlib::util::conf::error("\"" + e.name + "\" is not a location directive", e.line);
    }

    // Half a guard is the dangerous shape: a realm with no file would ask for
    // a password it could never check, and a file with no realm would send a
    // challenge no browser can answer.  Neither is a thing to guess at.
    if(g.realm.empty() && g.file.empty()) return;

    if(g.realm.empty()) wrong(d, "has auth_basic_user_file but no auth_basic");
    if(g.file.empty())  wrong(d, "has auth_basic but no auth_basic_user_file");

    o.protect.push_back(g);
}

/**
 * One `server { }`.
 *
 * **`server_name` takes a list**, as nginx's does, and one site is registered
 * per name -- they differ only in what a request has to say to reach them.
 * This is what Apache writes as ServerName plus ServerAlias, and a real
 * config has plenty: six names for one root is an ordinary amount, and
 * without a list that is six near-identical blocks whose only difference is
 * a string, which is a shape that invites a copy-paste mistake.
 *
 * Repeated `server_name` directives accumulate rather than the last one
 * winning, so a long list can be broken across lines the obvious way. The
 * alternative -- silently discarding the first -- is the kind of rule nobody
 * discovers until a name has been unreachable for a month.
 */
inline void apply_server(const jlib::util::conf::directive& d, options& o) {
    blocked(d, 0, 0);

    std::vector<std::string> names;
    site                     v;

    for(const jlib::util::conf::directive& e : d.block) {
        if(e.name == "server_name") {
            plain(e, 1, std::size_t(-1));

            for(std::size_t i = 0; i < e.args.size(); i++) {
                // A name given twice would register the same site twice,
                // which is harmless but means the operator believes something
                // that is not so -- most likely they meant a different name.
                for(std::size_t j = 0; j < names.size(); j++) {
                    if(names[j] == e.arg(i))
                        wrong(e, "lists \"" + e.arg(i) + "\" twice");
                }

                names.push_back(e.arg(i));
            }
        }
        else if(e.name == "root") { plain(e, 1, 1); v.root = e.arg(0); }
        else if(e.name == "ssl_certificate") { plain(e, 1, 1); v.cert = e.arg(0); }
        else if(e.name == "ssl_certificate_key") { plain(e, 1, 1); v.key = e.arg(0); }
        else throw jlib::util::conf::error("\"" + e.name + "\" is not a server directive", e.line);
    }

    if(names.empty()) wrong(d, "needs a server_name");
    if(v.root.empty()) wrong(d, "needs a root");

    // The same pairing rule the flag enforces, for the same reason: one half
    // of a certificate cannot be used and must not be ignored.
    if(v.cert.empty() != v.key.empty())
        wrong(d, "needs both ssl_certificate and ssl_certificate_key, or neither");

    for(std::size_t i = 0; i < names.size(); i++) {
        v.name = names[i];

        // Every name gets the certificate, because SNI matches on the name
        // the client asked for -- a site reachable by two names and holding a
        // certificate for only one of them would fail the handshake for the
        // other, before any of this is consulted.
        o.vhosts.push_back(v);
    }
}

inline void apply_http(const jlib::util::conf::directive& d, options& o) {
    blocked(d, 0, 0);

    for(const jlib::util::conf::directive& e : d.block) {
        // Two, not three: `ssl` and `redirect` are the only keywords and they
        // are mutually exclusive, so no legal `listen` has three arguments --
        // and keeping the bound tight is what lets a forgotten ";" be reported
        // as one.  `listen 8080` followed by `root /srv;` is three arguments,
        // and the arity message names the cause; a wider bound would let it
        // through to be refused as an unknown keyword instead.
        if(e.name == "listen") { plain(e, 1, 2); as_listen(e, o); }
        else if(e.name == "root") { plain(e, 1, 1); o.root = e.arg(0); }
        else if(e.name == "prefix") { plain(e, 1, 1); o.prefix = e.arg(0); }
        else if(e.name == "access_log") { plain(e, 1, 1); o.access_log = e.arg(0); }
        else if(e.name == "log_format") {
            plain(e, 1, 1);

            if(!log_shape_named(e.arg(0), o.log_format))
                throw jlib::util::conf::error(
                    "\"" + e.arg(0) + "\" is not a log format; "
                    "combined or vhost_combined", e.line);
        }
        else if(e.name == "cache_control") { plain(e, 1, 1); o.cache_control = e.arg(0); }
        else if(e.name == "index") {
            // Zero arguments is legal and means "no index", which is the only
            // way to ask for the old behaviour now that a directory is served.
            plain(e, 0, std::size_t(-1));

            o.index.assign(e.args.begin(), e.args.end());
        }
        else if(e.name == "ssl_certificate") { plain(e, 1, 1); o.cert = e.arg(0); }
        else if(e.name == "ssl_certificate_key") { plain(e, 1, 1); o.key = e.arg(0); }
        else if(e.name == "request_rate") { plain(e, 1, 1); o.rate = as_number(e, 0); }
        else if(e.name == "request_burst") { plain(e, 1, 1); o.burst = as_number(e, 0); }
        else if(e.name == "max_per_address") { plain(e, 1, 1); o.max_per_address = as_count(e, 0); }
        else if(e.name == "max_connections") { plain(e, 1, 1); o.max_connections = as_count(e, 0); }
        else if(e.name == "keepalive_requests") { plain(e, 1, 1); o.max_requests = as_count(e, 0); }
        else if(e.name == "keepalive_timeout") { plain(e, 1, 1); o.idle_timeout = as_number(e, 0); }
        else if(e.name == "client_header_timeout") { plain(e, 1, 1); o.initial_idle_timeout = as_number(e, 0); }
        else if(e.name == "io_timeout") { plain(e, 1, 1); o.io_timeout = as_number(e, 0); }
        else if(e.name == "location") apply_location(e, o);
        else if(e.name == "server") apply_server(e, o);
        else throw jlib::util::conf::error("\"" + e.name + "\" is not an http directive", e.line);
    }

    // **The end-of-walk checks**, which have to be here rather than in
    // as_listen: `ssl_certificate` may legitimately come after the `listen`
    // that needs it, so nothing about TLS can be decided until the block is
    // read.  The comment in as_listen promised this and it did not exist.
    if(o.cert.empty() != o.key.empty()) {
        wrong(d, o.cert.empty()
                     ? "has ssl_certificate_key but no ssl_certificate"
                     : "has ssl_certificate but no ssl_certificate_key");
    }

    bool any_ssl = false;

    for(std::size_t i = 0; i < o.listens.size(); i++) {
        if(o.listens[i].ssl) any_ssl = true;
    }

    // `listen 443 ssl;` with nothing to present is a configuration that cannot
    // work: the handshake would fail on every connection, and it would fail at
    // the first client rather than at startup where somebody is watching.
    if(any_ssl && o.cert.empty()) {
        throw jlib::util::conf::error(
            "\"listen ... ssl\" needs ssl_certificate and ssl_certificate_key",
            d.line);
    }

    for(std::size_t i = 0; i < o.listens.size(); i++) {
        if(!o.listens[i].redirect) continue;

        // Refused rather than ignored.  A redirect with nowhere to point is
        // the shape that silently serves nothing: every request to that port
        // would be answered with a Location naming a port nobody is listening
        // on, which looks like the server is up and is worse than a refusal.
        if(!any_ssl) {
            throw jlib::util::conf::error(
                "\"listen ... redirect\" needs a \"listen ... ssl\" to point at",
                o.listens[i].line);
        }
    }
}

/**
 * Read `path` into `o`.
 *
 * Throws jlib::util::conf::error, which carries the line. Nothing here warns and
 * continues: a directive nobody recognises is a typo, and a typo in a config
 * file is how a server ends up not doing the thing its operator believes it
 * is doing. Refusing to start is the only outcome that cannot be missed.
 */
/**
 * What a reload cannot change, given the old options and the new ones.
 *
 * SIGHUP re-reads the file, but most of what is in it was consumed at
 * startup and cannot be revisited without one: a port is bound, a route table
 * is registered, a thread pool is sized, privileges are dropped. Changing
 * those in a running process means rebuilding it, which means dropping every
 * connection -- and a reload that drops connections is a restart wearing a
 * different name.
 *
 * So the rule is: **apply what can be applied, and say plainly what was
 * ignored.** Silently keeping the old value is the one outcome that must not
 * happen, because the operator has a file on disk that does not describe the
 * server that is running, and nothing told them.
 *
 * @return the directive names that differ and need a restart, in config order
 */
inline std::vector<std::string> needs_a_restart(const options& was,
                                                const options& now)
{
    std::vector<std::string> changed;

    if(was.listens.size() != now.listens.size()) changed.push_back("listen");
    else {
        for(std::size_t i = 0; i < was.listens.size(); i++) {
            if(was.listens[i].port == now.listens[i].port &&
               was.listens[i].host == now.listens[i].host &&
               was.listens[i].ssl == now.listens[i].ssl &&
               was.listens[i].redirect == now.listens[i].redirect) continue;

            changed.push_back("listen");
            break;
        }
    }

    if(was.root != now.root)                   changed.push_back("root");
    if(was.prefix != now.prefix)               changed.push_back("prefix");
    if(was.index != now.index)                 changed.push_back("index");
    if(was.cache_control != now.cache_control) changed.push_back("cache_control");

    // A path change, not a rotation.  Rotation is moving the file out from
    // under the same path, which SIGHUP already handles by reopening it.
    if(was.access_log != now.access_log)       changed.push_back("access_log");
    if(was.error_log != now.error_log)         changed.push_back("error_log");
    if(was.log_format != now.log_format)       changed.push_back("log_format");

    if(was.threads != now.threads)             changed.push_back("threads");
    if(was.async != now.async)                 changed.push_back("async");
    if(was.user != now.user)                   changed.push_back("user");
    if(was.group != now.group)                 changed.push_back("user");
    if(was.pidfile != now.pidfile)             changed.push_back("pid");
    if(was.daemon != now.daemon)               changed.push_back("daemon");

    // Fixed in the policy the server was constructed with.
    if(was.max_connections != now.max_connections) changed.push_back("max_connections");
    if(was.max_per_address != now.max_per_address) changed.push_back("max_per_address");
    if(was.max_requests != now.max_requests)       changed.push_back("keepalive_requests");
    if(was.idle_timeout != now.idle_timeout)       changed.push_back("keepalive_timeout");
    if(was.initial_idle_timeout != now.initial_idle_timeout)
        changed.push_back("client_header_timeout");
    if(was.io_timeout != now.io_timeout)           changed.push_back("io_timeout");

    // A site is a route registration, so adding, removing or repointing one
    // needs the table rebuilt.  Its *certificate* is reloadable and is not
    // compared here -- see the note on reload().
    if(was.vhosts.size() != now.vhosts.size()) changed.push_back("server");
    else {
        for(std::size_t i = 0; i < was.vhosts.size(); i++) {
            if(was.vhosts[i].name == now.vhosts[i].name &&
               was.vhosts[i].root == now.vhosts[i].root) continue;

            changed.push_back("server");
            break;
        }
    }

    // Likewise a guard: which paths are protected and with what realm is a
    // registration.  The credential *file* is re-read on every reload, which
    // is the half that matters -- adding a user should not need a restart.
    if(was.protect.size() != now.protect.size()) changed.push_back("location");
    else {
        for(std::size_t i = 0; i < was.protect.size(); i++) {
            if(was.protect[i].prefix == now.protect[i].prefix &&
               was.protect[i].realm == now.protect[i].realm &&
               was.protect[i].file == now.protect[i].file) continue;

            changed.push_back("location");
            break;
        }
    }

    return changed;
}

/**
 * Every root the config serves, the default one first.
 *
 * So --test can walk them without repeating how a default site and a named
 * one differ -- which is only that one of them answers for a name nobody
 * claimed.
 */
inline std::vector<site> every_root(const options& o) {
    std::vector<site> all;
    site              first;

    first.name = "(default)";
    first.root = o.root;

    all.push_back(first);

    for(std::size_t i = 0; i < o.vhosts.size(); i++) all.push_back(o.vhosts[i]);

    return all;
}

inline void read_config(const std::string& path, options& o) {
    const std::vector<jlib::util::conf::directive> top = jlib::util::conf::read(path);

    bool saw_http = false;

    for(const jlib::util::conf::directive& d : top) {
        if(d.name == "http") {
            if(saw_http) wrong(d, "appears more than once");

            saw_http = true;

            apply_http(d, o);
        }
        else if(d.name == "user") {
            plain(d, 1, 2);

            o.user = d.arg(0);

            // nginx's `user NAME GROUP;`.  One argument means the group is
            // whatever resolving the user gives, which sys::resolve_identity
            // already does.
            if(d.args.size() > 1) o.group = d.arg(1);
        }
        else if(d.name == "daemon") { plain(d, 0, 1); o.daemon = as_flag(d); }
        else if(d.name == "async") { plain(d, 0, 1); o.async = as_flag(d); }
        else if(d.name == "allow_root") { plain(d, 0, 1); o.allow_root = as_flag(d); }
        else if(d.name == "pid") { plain(d, 1, 1); o.pidfile = d.arg(0); }
        else if(d.name == "threads") { plain(d, 1, 1); o.threads = unsigned(as_count(d, 0)); }
        else if(d.name == "error_log") {
            // Two arguments, nginx's shape: a path and an optional level.
            plain(d, 1, 2);

            o.error_log = d.arg(0);

            if(d.args.size() == 2 && !level_named(d.arg(1), o.error_level))
                throw jlib::util::conf::error(
                    "\"" + d.arg(1) + "\" is not a log level", d.line);
        }
        else throw jlib::util::conf::error("\"" + d.name + "\" is not a directive here", d.line);
    }
}

/**
 * How many times SIGHUP has arrived.
 *
 * A counter rather than a flag, so a signal landing between a reader's test
 * and its clear cannot be lost: each reader keeps the value it last acted on.
 * The log files notice it when they next write; the reload thread has
 * sys::wakeup to block on instead.
 *
 * Now a thin read of sys::wakeup rather than its own variable, so there is one
 * handler in the process and one place that counts.
 */
inline std::sig_atomic_t hups() { return jlib::sys::wakeup::count(SIGHUP); }


inline bool logfile::open(const std::string& path) {
    if(path.empty()) return true;

    m_path = path;
    m_acted_on = hups();

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
    const std::sig_atomic_t asked = hups();

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
