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

#ifdef HAVE_PWHASH
#include <sodium.h>
#endif

#include <sys/stat.h>

#include <stdexcept>

#include <csignal>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <future>
#include <map>
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

    std::size_t size() const { return m_hashes.size(); }

private:
    std::map<std::string, std::string> m_hashes;

    // What an unknown user is checked against.  See check().
    std::string m_decoy;
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

    m_hashes.clear();

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

        m_hashes[line.substr(0, colon)] = line.substr(colon + 1);
    }

    if(m_hashes.empty())
        throw std::runtime_error("\"" + path + "\" has no credentials in it");

    // The decoy, at the same parameters as everything else, over a password
    // nobody can send: 32 random bytes rather than a word somebody might.
    unsigned char noise[32];

    ::randombytes_buf(noise, sizeof noise);

    m_decoy = hash_password(std::string(reinterpret_cast<char*>(noise),
                                        sizeof noise));
#endif
}

inline bool credentials::check(const std::string& user,
                               const std::string& password) const
{
#ifndef HAVE_PWHASH
    (void)user; (void)password;

    return false;
#else
    const std::map<std::string, std::string>::const_iterator i =
        m_hashes.find(user);

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
    const std::string& against = i == m_hashes.end() ? m_decoy : i->second;

    const int ok = ::crypto_pwhash_str_verify(against.c_str(), password.data(),
                                              password.size());

    // The verification itself is constant-time; this is only about not
    // letting the *lookup* leak.
    return i != m_hashes.end() && ok == 0;
#endif
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
