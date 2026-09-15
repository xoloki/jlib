/* -*- mode: C++ c-basic-offset: 4 -*-
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
 */

#include <jlib/net/http_server.hh>

#include <jlib/util/abnf.hh>
#include <jlib/util/MimeType.hh>

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <vector>

#include <limits.h>
#include <sys/stat.h>
#include <stdlib.h>

#include <cstring>
#include <cstdlib>

#include <jlib/sys/await.hh>

#include <exception>

#include <jlib/util/URL.hh>
#include <jlib/util/util.hh>

#include <ctime>
#include <sstream>

namespace jlib {
namespace net {
namespace http {

/** "jlib/" and the release.  See net::http::default_user_agent. */
std::string default_server_name() { return "jlib/" JLIB_RELEASE_STRING; }

namespace {

    /**
     * One chunk, as RFC 9112 7.1 spells it: the size in hex, CRLF, the octets,
     * CRLF.
     *
     * Lower-case hex and no chunk extensions -- both are what the grammar in
     * rfc9112.hh accepts, and this server's own reader parses what this writes
     * with `chunk-size` out of that same grammar.  Writing something its
     * reader would refuse is the failure mode worth ruling out by
     * construction.
     */
    std::string chunk(const std::string& piece) {
        std::ostringstream o;

        o << std::hex << piece.size() << "\r\n" << piece << "\r\n";

        return o.str();
    }

    /** The terminating zero-length chunk, and an empty trailer section. */
    std::string last_chunk() { return "0\r\n\r\n"; }

}

namespace {

    /** RFC 9110 5.6.7's IMF-fixdate, which is fixed-format and never localised. */
    int month_of(const std::string& name) {
        static const char* const MON[] = { "Jan", "Feb", "Mar", "Apr", "May",
                                           "Jun", "Jul", "Aug", "Sep", "Oct",
                                           "Nov", "Dec" };

        for(int i = 0; i < 12; i++) {
            if(name == MON[i]) return i;
        }

        return -1;
    }

    /** Digits the grammar has already vouched for. */
    long number(const std::string& s) {
        return std::strtol(s.c_str(), 0, 10);
    }

    /**
     * Parse an HTTP-date, against RFC 9110 5.6.7's grammar.
     *
     * **All three forms**, because the grammar is pasted in rfc9110.hh and
     * says so: IMF-fixdate, and the two obsolete ones a recipient MUST still
     * accept -- RFC 850's `Sunday, 06-Nov-94` and asctime's `Sun Nov  6`.
     *
     * The first version of this counted characters -- `s[3] != ','`,
     * `s.compare(25, 4, " GMT")` -- and accepted only the modern form, with a
     * note explaining why the other two were a safe thing to get wrong.  That
     * note was unnecessary: the grammar was already in the tree, already
     * compiled into util::http::grammar(), and reading it is both more correct
     * and less code than the offsets were.
     *
     * The three alternatives are tried separately rather than through the
     * `HTTP-date` rule above them, because their captures do not line up: a
     * two-digit year has no rule of its own in `date2`, and asctime keeps its
     * day inside `date3`.  Each branch below takes what its own shape names.
     */
    bool parse_http_date(const std::string& s, std::time_t& out) {
        const util::abnf::grammar& g = util::http::grammar();

        std::tm tm;

        std::memset(&tm, 0, sizeof tm);

        long year = -1;
        long day = -1;

        const util::abnf::parse_result imf = g.at("IMF-fixdate").try_parse(s);

        if(imf) {
            const util::abnf::match m = imf.root();

            day = number(m["day"].str());
            year = number(m["year"].str());
            tm.tm_mon = month_of(m["month"].str());
            tm.tm_hour = int(number(m["hour"].str()));
            tm.tm_min = int(number(m["minute"].str()));
            tm.tm_sec = int(number(m["second"].str()));
        }
        else {
            const util::abnf::parse_result old = g.at("rfc850-date").try_parse(s);

            if(old) {
                const util::abnf::match m = old.root();
                const std::string d2 = m["date2"].str();

                // `date2 = day "-" month "-" 2DIGIT`, and that last 2DIGIT is
                // the one piece of any of these with no rule to name it.  Taken
                // from the end of a string the grammar has already shaped,
                // which is a different thing from indexing into input nobody
                // has checked.
                if(d2.size() < 2) return false;

                day = number(m["day"].str());
                tm.tm_mon = month_of(m["month"].str());
                tm.tm_hour = int(number(m["hour"].str()));
                tm.tm_min = int(number(m["minute"].str()));
                tm.tm_sec = int(number(m["second"].str()));

                const long two = number(d2.substr(d2.size() - 2));

                // RFC 6265 5.1.1's rule, which is the only one anybody wrote
                // down: a two-digit year is 20xx below 70 and 19xx otherwise.
                year = two < 70 ? 2000 + two : 1900 + two;
            }
            else {
                const util::abnf::parse_result asc = g.at("asctime-date").try_parse(s);

                if(!asc) return false;

                const util::abnf::match m = asc.root();
                const std::string d3 = m["date3"].str();

                // `date3 = month SP ( 2DIGIT / ( SP DIGIT ) )` -- the day is
                // whatever follows the month and a space, with the single-digit
                // form padded rather than shortened.
                const std::size_t sp = d3.find(' ');

                if(sp == std::string::npos) return false;

                day = number(d3.substr(sp + 1));
                year = number(m["year"].str());
                tm.tm_mon = month_of(m["month"].str());
                tm.tm_hour = int(number(m["hour"].str()));
                tm.tm_min = int(number(m["minute"].str()));
                tm.tm_sec = int(number(m["second"].str()));
            }
        }

        if(tm.tm_mon < 0 || day < 1 || day > 31 || year < 1900) return false;

        tm.tm_mday = int(day);
        tm.tm_year = int(year) - 1900;

        const std::time_t when = ::timegm(&tm);

        if(when == std::time_t(-1)) return false;

        out = when;

        return true;
    }

    /** Split a field value on commas, trimming each part. */
    std::vector<std::string> comma_list(const std::string& v) {
        std::vector<std::string> out;
        std::string cur;

        for(std::size_t i = 0; i <= v.size(); i++) {
            if(i == v.size() || v[i] == ',') {
                std::size_t a = 0, b = cur.size();

                while(a < b && (cur[a] == ' ' || cur[a] == '\t')) a++;
                while(b > a && (cur[b - 1] == ' ' || cur[b - 1] == '\t')) b--;

                if(b > a) out.push_back(cur.substr(a, b - a));

                cur.clear();
            }
            else {
                cur += v[i];
            }
        }

        return out;
    }

    /**
     * RFC 9110 8.8.3.2, the weak comparison, which is the one If-None-Match
     * uses: `W/"x"` and `"x"` are the same entity-tag for this purpose.
     *
     * Only If-Match and If-Range want the strong one, and neither is here.
     */
    bool same_etag_weakly(const std::string& a, const std::string& b) {
        const std::string x = a.compare(0, 2, "W/") == 0 ? a.substr(2) : a;
        const std::string y = b.compare(0, 2, "W/") == 0 ? b.substr(2) : b;

        return x == y;
    }

    /**
     * Should this response become a 304?  RFC 9110 13.1.
     *
     * If-None-Match wins outright where both are present -- 13.1.3 says a
     * recipient MUST ignore If-Modified-Since when If-None-Match is there,
     * because an entity-tag says more than a second-resolution timestamp can.
     */
    bool not_modified(const util::http::Request& q,
                      const server::response& r)
    {
        if(q.fields().has("If-None-Match")) {
            const std::string want = q.fields().get("If-None-Match");

            // "*" means "if any representation exists", and one does: this is
            // being asked about a response the handler has already produced.
            if(util::http::fold(want) == "*") return true;

            if(!r.fields().has("ETag")) return false;

            const std::string have = r.fields().get("ETag");

            for(const std::string& one : comma_list(want)) {
                if(same_etag_weakly(one, have)) return true;
            }

            return false;
        }

        if(q.fields().has("If-Modified-Since") && r.fields().has("Last-Modified")) {
            std::time_t since = 0;
            std::time_t last = 0;

            if(!parse_http_date(q.fields().get("If-Modified-Since"), since))
                return false;

            if(!parse_http_date(r.fields().get("Last-Modified"), last))
                return false;

            // Not *newer* than: equal counts as unmodified, which is what
            // makes a second request with the value just handed out a 304.
            return last <= since;
        }

        return false;
    }

    /**
     * Turn a response into a 304, keeping only what one may carry.
     *
     * RFC 9110 15.4.5: a 304 has no content, and sends the metadata that would
     * have gone with a 200 -- the validators above all, because a client that
     * got a 304 and lost its ETag would have to ask again without one.
     */
    void make_not_modified(server::response& r) {
        r.status(304).body(std::string());
    }

    std::string http_date(std::time_t when) {
        static const char* const DAY[] = { "Sun", "Mon", "Tue", "Wed", "Thu",
                                           "Fri", "Sat" };
        static const char* const MONTH[] = { "Jan", "Feb", "Mar", "Apr", "May",
                                             "Jun", "Jul", "Aug", "Sep", "Oct",
                                             "Nov", "Dec" };

        const std::time_t now = when;

        std::tm tm;

        // Spelled out rather than strftime, because strftime's %a and %b are
        // whatever the locale says and RFC 9110 requires these exact names.
        if(::gmtime_r(&now, &tm) == 0) return std::string();

        char buf[64];

        std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                      DAY[tm.tm_wday % 7], tm.tm_mday, MONTH[tm.tm_mon % 12],
                      tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);

        return buf;
    }

    /**
     * The media type for a name, by extension.
     *
     * **Not util::MimeType**, which exists and is the obvious thing to reach
     * for -- it runs file(1) as a subprocess.  That is right for classifying
     * an attachment somebody mailed you and wrong for answering a request:
     * a fork and an exec per file, on a path where the answer is already
     * determined by three characters at the end of the name.
     *
     * Content sniffing is also less accurate here rather than more.  An empty
     * .js is "inode/x-empty" to file(1) and application/javascript to a
     * browser, and the browser is the one being answered.
     *
     * Unknown means application/octet-stream, which is the one answer that
     * cannot be wrong in a dangerous direction: a browser will not execute it.
     */
    std::string type_by_extension(const std::string& name) {
        static const struct { const char* ext; const char* type; } TABLE[] = {
            { ".html", "text/html; charset=utf-8" },
            { ".htm",  "text/html; charset=utf-8" },
            { ".css",  "text/css; charset=utf-8" },
            { ".js",   "application/javascript; charset=utf-8" },
            { ".mjs",  "application/javascript; charset=utf-8" },
            { ".json", "application/json" },
            { ".txt",  "text/plain; charset=utf-8" },
            { ".md",   "text/markdown; charset=utf-8" },
            { ".xml",  "application/xml" },
            { ".svg",  "image/svg+xml" },
            { ".png",  "image/png" },
            { ".jpg",  "image/jpeg" },
            { ".jpeg", "image/jpeg" },
            { ".gif",  "image/gif" },
            { ".webp", "image/webp" },
            { ".ico",  "image/vnd.microsoft.icon" },
            { ".woff", "font/woff" },
            { ".woff2","font/woff2" },
            { ".wasm", "application/wasm" },
            { ".pdf",  "application/pdf" },
            { ".wav",  "audio/wav" },
            { ".mp3",  "audio/mpeg" },
            { ".mp4",  "video/mp4" }
        };

        const std::size_t dot = name.find_last_of('.');

        if(dot != std::string::npos) {
            const std::string ext = util::http::fold(name.substr(dot));

            for(std::size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
                if(ext == TABLE[i].ext) return TABLE[i].type;
            }
        }

        return "application/octet-stream";
    }

    /**
     * A validator from what stat() knows.
     *
     * **Weak, and that is not timidity.**  RFC 9110 8.8.1: a strong validator
     * changes whenever the bytes do.  Modification time at one-second
     * resolution and a size do not: a generated file rewritten twice in the
     * same second to the same length has the same pair and different content.
     * nginx and Apache send this as strong anyway; jlib says what it can
     * actually promise, which is equivalence rather than identity.
     *
     * Weak costs nothing here.  If-None-Match compares weakly (8.8.3.2), which
     * is what this is for.  It would matter for If-Range, which needs a strong
     * one -- and If-Range is not implemented, so when it is, this is where the
     * conversation about hashing starts.
     */
    std::string etag_for(const struct stat& st) {
        std::ostringstream o;

        o << "W/\"" << std::hex << static_cast<long long>(st.st_mtime) << "-"
          << std::hex << static_cast<long long>(st.st_size) << "\"";

        return o.str();
    }

    /** 404 for everything, deliberately; see server::files. */
    void nothing_there(server::response& r) {
        r.status(404).type("text/plain").body("not found\n");
    }

    /**
     * What a client asked for, once the arithmetic is done.
     *
     * `first` and `last` are inclusive, as they are on the wire: RFC 9110 14.1
     * counts `bytes=0-0` as one octet, not none.
     */
    struct byte_range {
        long long first = 0;
        long long last = 0;

        long long length() const { return last - first + 1; }
    };

    enum range_outcome {
        range_absent,        // no Range, or one deliberately ignored
        range_ok,            // 206, and `out` says which octets
        range_unsatisfiable  // 416
    };

    /**
     * Read a Range against RFC 9110 14.1's grammar and work out what it means.
     *
     * ## What is deliberately ignored, and why that is allowed
     *
     * 14.2: "A server MAY ignore the Range header field."  Three things are
     * ignored here, and each becomes a plain 200 rather than an error:
     *
     *   - a unit that is not `bytes`.  There are no others in practice and
     *     `other-range` exists in the grammar precisely because the RFC
     *     declines to define any.
     *   - **more than one range.**  Answering two means multipart/byteranges,
     *     a whole body format with its own boundaries, to save a round trip
     *     for a client that could have asked twice.  The cost is not in
     *     proportion to the benefit and the RFC's permission is explicit.
     *   - anything the grammar refuses.
     *
     * ## What is not ignored
     *
     * A single satisfiable range is answered, and a single *unsatisfiable* one
     * is refused with 416 rather than quietly answered with the whole file --
     * 14.4 requires that, and it is the difference between a client learning
     * its offset is past the end and a client silently re-reading everything.
     *
     * A zero-length file has no satisfiable range at all, which is why the
     * suffix case tests it separately.
     */
    range_outcome decide_range(const util::http::Request& q, long long size,
                               byte_range& out)
    {
        if(!q.fields().has("Range")) return range_absent;

        const util::abnf::grammar& g = util::http::grammar();
        const std::string value = q.fields().get("Range");

        const util::abnf::parse_result p =
            g.at("ranges-specifier").try_parse(value);

        if(!p) return range_absent;

        const util::abnf::match m = p.root();

        if(util::http::fold(m["range-unit"].str()) != "bytes")
            return range_absent;

        // One only.  A comma in the set means the client asked for several,
        // and the answer to that is the whole representation.
        if(m["range-set"].str().find(',') != std::string::npos)
            return range_absent;

        const std::string spec = m["range-spec"].str();

        if(spec.empty()) return range_absent;

        if(spec[0] == '-') {
            // suffix-range: the last N octets.  N of zero asks for nothing,
            // which is unsatisfiable rather than empty -- 14.1.2.
            const long long want = number(spec.substr(1));

            if(want <= 0 || size == 0) return range_unsatisfiable;

            out.first = want >= size ? 0 : size - want;
            out.last = size - 1;

            return range_ok;
        }

        const std::size_t dash = spec.find('-');

        if(dash == std::string::npos) return range_absent;

        out.first = number(spec.substr(0, dash));

        const std::string tail = spec.substr(dash + 1);

        // "first-" means to the end.  Clamped rather than refused when the
        // client asks past it, because 14.1.2 says a last-pos beyond the
        // representation is the representation's end.
        out.last = tail.empty() ? size - 1 : number(tail);

        if(out.last >= size) out.last = size - 1;

        // A first-pos past the end lands here rather than in a test of its
        // own: clamping has already pulled last-pos back to the last octet, so
        // it is now below first-pos.  An explicit `first >= size` check was
        // written and could not be broken -- this line had caught every case
        // it was meant to.
        if(out.last < out.first) return range_unsatisfiable;

        return range_ok;
    }

    /**
     * Does If-Range allow the range to be answered?  RFC 9110 13.1.5.
     *
     * **Strong comparison, which is the whole point of the field.**  A client
     * sends the validator it holds and asks for a piece only if the thing has
     * not changed underneath it; a weak tag says two representations are
     * equivalent for display, which is not good enough to staple half of one
     * onto half of another.
     *
     * A consequence worth stating plainly: `files()` sends `W/"mtime-size"`,
     * deliberately, because mtime at second resolution and a size cannot
     * promise the bytes are identical.  A weak tag can never satisfy strong
     * comparison, so **a conditional range against a served file always gets
     * the whole file back**.  That is correct and conservative -- the client
     * re-reads rather than splicing two different files together -- and it is
     * the cost of the honest validator.  Plain Range, which is what a media
     * player seeking actually sends, is unaffected.
     */
    bool range_still_current(const util::http::Request& q,
                             const server::response& r)
    {
        if(!q.fields().has("If-Range")) return true;

        const std::string want = q.fields().get("If-Range");

        // A date, not an entity-tag.  Compared exactly: 13.1.5 allows it only
        // when the origin can be sure the representation has not changed
        // within the second, and equality is the strongest thing available
        // here.
        if(!want.empty() && want[0] != '"' && want.compare(0, 2, "W/") != 0) {
            std::time_t asked = 0;
            std::time_t have = 0;

            if(!parse_http_date(want, asked)) return false;

            if(!r.fields().has("Last-Modified")) return false;

            if(!parse_http_date(r.fields().get("Last-Modified"), have))
                return false;

            return asked == have;
        }

        if(!r.fields().has("ETag")) return false;

        const std::string have = r.fields().get("ETag");

        // **Strong comparison, as one test rather than two.**  Written as a
        // check per side at first, and neither could be broken on its own:
        // a weak tag differs from a strong one by the `W/` it carries, so the
        // exact compare below already separates those.  What the compare
        // cannot catch is two *weak* tags that are equal -- which is precisely
        // the files() case, and precisely what 8.8.3.2 says must not authorise
        // a range.  One condition, one thing it is for.
        if(want.compare(0, 2, "W/") == 0 || have.compare(0, 2, "W/") == 0)
            return false;

        return want == have;
    }

    /**
     * Apply a Range to a response a handler has already produced.
     *
     * **Range is not only for files.**  A buffered handler has its whole body
     * in hand, so answering a range is slicing it -- and a client seeking in
     * something generated has the same right to ask as one seeking in a file.
     * `files()` does not come through here: it seeks instead of slicing, which
     * is the point of streaming it.
     *
     * Only for a 200 with a body, and only for a safe method, for the reason
     * 13.1 gives about conditional requests generally: a range of a 404 is not
     * a thing, and a range of a POST's answer is not a meaning the RFC
     * defines.
     */
    void apply_range(const util::http::Request& q, server::response& r) {
        if(r.status() != 200) return;

        const long long size = static_cast<long long>(r.body().size());

        if(size == 0) return;

        if(!range_still_current(q, r)) return;

        byte_range span;

        const range_outcome what = decide_range(q, size, span);

        if(what == range_absent) return;

        if(what == range_unsatisfiable) {
            std::ostringstream cr;

            cr << "bytes */" << size;

            r = server::response();

            r.status(416).type("text/plain")
             .field("Content-Range", cr.str())
             .field("Accept-Ranges", "bytes")
             .body("that range is not satisfiable\n");

            return;
        }

        std::ostringstream cr;

        cr << "bytes " << span.first << "-" << span.last << "/" << size;

        // The body is replaced, and the Content-Length with it -- which is
        // computed from the body at serialisation, so slicing is enough.
        r.status(206)
         .field("Content-Range", cr.str())
         .body(r.body().substr(std::size_t(span.first),
                               std::size_t(span.length())));
    }

    /** How much of a file is held at once.  See stream_file. */
    const std::size_t FILE_BLOCK = 64 * 1024;

    struct located {
        std::string real;
        struct stat st;
    };

    /**
     * Resolve a request into a file under the root, or refuse.
     *
     * Containment is decided by realpath, not by inspecting the path: a string
     * check catches ".." and misses a symlink, and resolving first catches
     * both because what comes back is where the kernel would actually go.
     */
    bool locate(const std::string& root, const std::string& rest, located& f) {
        const std::string candidate = root + "/" + rest;

        char resolved[PATH_MAX];

        if(::realpath(candidate.c_str(), resolved) == 0) return false;

        f.real = resolved;

        // Under the root, or the root itself.  The trailing separator matters:
        // without it "/srv/wwwroot-evil" is inside "/srv/www".
        if(f.real != root &&
           f.real.compare(0, root.size() + 1, root + "/") != 0) {
            return false;
        }

        // Regular files only.  This is also what refuses the prefix itself: an
        // empty rest resolves to the root, which is a directory, so there is
        // no listing and no index.html -- both being decisions a caller should
        // make out loud rather than ones this makes quietly.
        if(::stat(f.real.c_str(), &f.st) != 0 || !S_ISREG(f.st.st_mode))
            return false;

        return true;
    }

    /**
     * Everything that can fail, and everything that shapes the head, before a
     * byte goes out.
     *
     * A streaming response gives up the promise the buffered one makes -- that
     * a handler which fails is still answered -- because the status has
     * already gone.  For a file that promise is kept anyway by deciding first:
     * the path resolves or it does not, the validators come from `stat()`
     * rather than from the body, and the range arithmetic needs only the size.
     * So 404, 304, 416 and 206 are all still decided here.
     *
     * **The response is built once, at the end.**  The first version of this
     * set Content-Length while building a 200 head and then set it again for a
     * 206 -- and `fields::add` appends, so that response carried two
     * Content-Lengths that disagreed, which is exactly the shape
     * `util::http::decide_framing` refuses as a smuggling primitive.  The
     * server would have been emitting what its own reader throws out.
     *
     * @param span  which octets to stream; the whole file unless a range said
     *              otherwise
     * @return      false if `r` is the whole answer and nothing should follow
     */
    bool file_decided(const std::string& root, const std::string& rest,
                      const util::http::Request& q, located& f,
                      server::response& r, byte_range& span)
    {
        if(!locate(root, rest, f)) {
            nothing_there(r);

            return false;
        }

        const long long size = static_cast<long long>(f.st.st_size);

        // The validators alone, for the two conditional questions below.  They
        // are also what a 304 has to carry, which is why this is the object
        // that becomes one.
        server::response probe;

        probe.field("Last-Modified", http_date(f.st.st_mtime))
             .field("ETag", etag_for(f.st));

        // **Before the range**, because 13.1 orders them that way and because
        // the answers differ: a client whose copy is current wants a 304, not
        // a piece of something it already has.
        if(not_modified(q, probe)) {
            r = probe;

            make_not_modified(r);

            return false;
        }

        span.first = 0;
        span.last = size > 0 ? size - 1 : 0;

        bool partial = false;

        // If-Range says "only if it has not changed".  A no turns the request
        // back into a plain one for the whole representation rather than an
        // error -- 13.1.5.
        if(range_still_current(q, probe)) {
            byte_range asked;

            const range_outcome what = decide_range(q, size, asked);

            if(what == range_unsatisfiable) {
                std::ostringstream cr;

                // 14.4's unsatisfied-range form, which tells the client how
                // long the representation actually is so its next ask can be
                // right.
                cr << "bytes */" << size;

                r = server::response();

                r.status(416).type("text/plain")
                 .field("Content-Range", cr.str())
                 .field("Accept-Ranges", "bytes")
                 .body("that range is not satisfiable\n");

                return false;
            }

            if(what == range_ok) {
                span = asked;
                partial = true;
            }
        }

        std::ostringstream len;

        len << (partial ? span.length() : size);

        r = server::response();

        r.status(partial ? 206 : 200)
         .type(type_by_extension(f.real))
         .field("Content-Length", len.str())
         .field("Last-Modified", http_date(f.st.st_mtime))
         .field("ETag", etag_for(f.st))
         // RFC 9110 14.3: advertised where a client looks before deciding
         // whether seeking is possible at all.
         .field("Accept-Ranges", "bytes");

        if(partial) {
            std::ostringstream cr;

            cr << "bytes " << span.first << "-" << span.last << "/" << size;

            r.field("Content-Range", cr.str());
        }

        return size > 0;
    }

    const char* reason_for(int status) {
        switch(status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default:  return "";
        }
    }

}

// ------------------------------------------------------------------ response

server::response& server::response::status(int code, const std::string& reason) {
    m_status = code;
    m_reason = reason.empty() ? reason_for(code) : reason;

    return *this;
}

server::response& server::response::field(std::string name, std::string value) {
    m_fields.add(std::move(name), std::move(value));

    return *this;
}

server::response& server::response::type(const std::string& content_type) {
    return field("Content-Type", content_type);
}

server::response& server::response::body(std::string body) {
    m_body = std::move(body);

    return *this;
}

std::string server::response::head(const std::string& server_name,
                                   bool chunked, bool persist) const
{
    return serialise(server_name, false, persist, chunked);
}

std::string server::response::str(const std::string& server_name,
                                  bool persist) const
{
    return serialise(server_name, true, persist) + m_body;
}

std::string server::response::without_body(const std::string& server_name,
                                           bool persist) const
{
    return serialise(server_name, true, persist);
}

std::string server::response::serialise(const std::string& server_name,
                                        bool with_length, bool persist,
                                        bool chunked) const
{
    std::ostringstream o;

    const std::string reason = m_reason.empty() ? reason_for(m_status) : m_reason;

    o << "HTTP/1.1 " << m_status << " " << reason << "\r\n";

    // Supplied unless the handler said otherwise, so a handler that wants to
    // lie about Content-Length -- which no correct one does -- has to say so.
    if(!m_fields.has("Date")) {
        const std::string when = http_date(std::time(0));

        if(!when.empty()) o << "Date: " << when << "\r\n";
    }

    if(!m_fields.has("Server") && !server_name.empty())
        o << "Server: " << server_name << "\r\n";

    // Omitted entirely when the body is not yet known -- a streaming response
    // is framed by Transfer-Encoding or by the close instead.  See
    // responder::begin.
    // RFC 9110 15.3.5 and 15.4.5: a 204 and a 304 have no content, and a
    // Content-Length on one is a promise of a body that is not coming -- which
    // on a reused connection is the next response being read as that body.
    const bool no_content = m_status == 204 || m_status == 304 ||
                            (m_status >= 100 && m_status < 200);

    if(with_length && !no_content && !m_fields.has("Content-Length"))
        o << "Content-Length: " << m_body.size() << "\r\n";

    // RFC 9112 6.1 forbids both at once and util::http::decide_framing refuses
    // a message carrying them, so a server that emitted both would be writing
    // what its own reader would throw out.  They are mutually exclusive above
    // and here by construction, not by checking.
    if(chunked && !m_fields.has("Transfer-Encoding"))
        o << "Transfer-Encoding: chunked\r\n";

    // Unless the handler said otherwise, and then what the caller decided.
    // Stated rather than left out: keep-alive is the HTTP/1.1 default, so
    // saying nothing would also mean persist, but a reader of a capture should
    // not have to know the version to know what this connection is doing.
    if(!m_fields.has("Connection"))
        o << "Connection: " << (persist ? "keep-alive" : "close") << "\r\n";

    for(const fields::value_type& f : m_fields) {
        // Checked on the way out, against the grammar rather than a blocklist.
        // A CR or an LF in a value is how one response becomes two, and a
        // handler builds these from whatever it was given.
        const std::string line = f.first + ": " + f.second;

        if(!util::http::grammar().at("field-line").try_parse(line))
            throw error("a handler produced a header field that cannot be sent: "
                        "\"" + line + "\"");

        o << f.first << ": " << f.second << "\r\n";
    }

    o << "\r\n";

    return o.str();
}

// -------------------------------------------------------------------- server

server::server(unsigned short port, const std::string& host,
               const sys::tls_context& tls, const sys::server::policy& p,
               const options& o)
    : m_options(o),
      m_tls(!tls.empty())
{
    m_otherwise = [](const Request&, response& r) {
        r.status(404).type("text/plain").body("not found\n");
    };

    m_transport.reset(new sys::server(
        sys::listener(port, host),
        [this](sys::socketstream& s, const sys::peer& from) { serve(s, from); },
        tls, p));
}

server::~server() = default;

unsigned short server::port() const { return m_transport->port(); }

bool server::tls() const { return m_tls; }

std::string server::url(const std::string& path) const {
    std::ostringstream o;

    // localhost rather than 127.0.0.1 for the TLS form, because a certificate
    // covers a name and the test ones cover that one.
    o << (m_tls ? "https://localhost:" : "http://127.0.0.1:") << port() << path;

    return o.str();
}

void server::route(const std::string& method, const std::string& path, handler h) {
    entry e;

    e.method = method;
    e.path = path;
    e.run = std::move(h);

    compile_route(e);

    m_routes.push_back(std::move(e));
}

void server::route(const std::string& method, const std::string& path,
                   stream_handler h)
{
    entry e;

    e.method = method;
    e.path = path;
    e.stream = std::move(h);

    compile_route(e);

    m_routes.push_back(std::move(e));
}

void server::files(const std::string& pattern, const std::string& root) {
    // Resolved once, at registration: a root that does not exist is a mistake
    // in the program rather than a 404 repeated per request, and resolving it
    // here is also what makes the containment check a string compare against
    // something already canonical.
    char resolved[PATH_MAX];

    if(::realpath(root.c_str(), resolved) == 0) {
        throw error("cannot serve files from \"" + root + "\": " +
                    std::strerror(errno));
    }

    const std::string real_root(resolved);

    // **Both kinds on one route**, which is why the two serves refuse the
    // other's handler only when their own is missing.  A file is streamed
    // either way and the two servers stream differently, so one registration
    // has to carry one of each or `files()` would work on whichever server the
    // caller did not have.
    entry e;

    e.method = "GET";
    e.path = pattern;

    e.param_stream =
        [real_root](const Request& q, const params& p, responder& out) {
            located f;
            response head;
            byte_range span;

            if(!file_decided(real_root, p.rest(), q, f, head, span)) {
                out.send(head);

                return;
            }

            out.begin(head);

            std::ifstream in(f.real.c_str(), std::ios::binary);

            if(!in) return;

            // **Seek, rather than read and discard.**  This is what the
            // streaming branch bought: a range costs the range, not the file.
            in.seekg(std::streamoff(span.first));

            if(!in) return;

            std::vector<char> block(FILE_BLOCK);

            // One block is held at a time rather than the file, so what a
            // request costs is FILE_BLOCK and not st_size.
            long long left = span.length();

            while(left > 0 && in) {
                const std::size_t want =
                    static_cast<std::size_t>(
                        std::min<long long>(left,
                                            static_cast<long long>(block.size())));

                in.read(&block[0], std::streamsize(want));

                const std::streamsize got = in.gcount();

                if(got <= 0) break;

                out.write(std::string(&block[0], std::size_t(got)));

                left -= got;

                // Nobody is reading; stop producing.  A large file to a client
                // that has gone is otherwise read in full for nothing.
                if(!out.live()) return;
            }
        };

    e.async_param_stream =
        [real_root](const Request& q, const params& p,
                    async_responder& out) -> sys::task<void> {
            located f;
            response head;
            byte_range span;

            if(!file_decided(real_root, p.rest(), q, f, head, span)) {
                co_await out.send(head);

                co_return;
            }

            co_await out.begin(head);

            std::ifstream in(f.real.c_str(), std::ios::binary);

            if(!in) co_return;

            // See the blocking half: a range costs the range.
            in.seekg(std::streamoff(span.first));

            if(!in) co_return;

            std::vector<char> block(FILE_BLOCK);

            long long left = span.length();

            while(left > 0) {
                // **Read on a worker, write on the reactor.**  A file read is
                // microseconds when the page is cached and a disk seek when it
                // is not, and the reactor's thread is the wrong place to find
                // out which.  This is what the pool is for -- short work with
                // a syscall in it -- as against a generation, which gets a
                // thread of its own; see sys::relay.
                const std::size_t want =
                    static_cast<std::size_t>(
                        std::min<long long>(left,
                                            static_cast<long long>(block.size())));

                co_await sys::on_pool(out.pool());

                in.read(&block[0], std::streamsize(want));

                const std::streamsize got = in.gcount();

                co_await sys::on_reactor(out.reactor());

                if(got <= 0) break;

                co_await out.write(std::string(&block[0], std::size_t(got)));

                left -= got;

                if(!in) break;
            }
        };

    compile_route(e);

    m_routes.push_back(std::move(e));
}

void server::otherwise(handler h) {
    if(h) m_otherwise = std::move(h);
}

bool server::serve_one(double timeout) { return m_transport->serve_one(timeout); }

void server::run() { m_transport->run(); }

void server::stop(bool drain) { m_transport->stop(drain); }

void server::join() { m_transport->join(); }

sys::server& server::transport() { return *m_transport; }

void server::responder::send(const response& r) {
    if(m_started)
        throw error("a responder sent a whole response after it had begun "
                    "streaming one");

    m_started = true;

    *m_s << r.str(m_name) << std::flush;
}

void server::responder::suppress_body() { m_no_body = true; }

void server::async_responder::suppress_body() { m_no_body = true; }

void server::responder::framing(bool chunked) {
    if(m_started)
        throw error("a responder was told how to frame a response it has "
                    "already begun");

    m_chunked = chunked;
}

void server::responder::begin(const response& head) {
    if(m_started)
        throw error("a responder began a response twice");

    m_started = true;

    // **A handler that knows the length says so, and then nothing needs
    // chunking.**  A body with a Content-Length already has an end; chunking
    // one would frame it twice and cost a header per piece for nothing.  This
    // is what lets a file be streamed and still tell the client how big it is.
    if(head.fields().has("Content-Length")) m_chunked = false;

    // The blocking server closes after one response whatever happens, so
    // persist is false here and chunked buys only the terminator -- which is
    // what tells a client the difference between a body that ended and a
    // server that died.  See the responder class comment.
    *m_s << head.head(m_name, m_chunked, false) << std::flush;

    // Nothing follows a HEAD, including the terminating chunk: the head said
    // how the body *would* have been framed, and then there is no body.
    if(m_no_body) m_ended = true;
}

void server::responder::write(const std::string& piece) {
    if(!m_started)
        throw error("a responder wrote a body piece before begin()");

    // A zero-length chunk is the terminator, so writing one here would end the
    // body early and the client would believe it complete.  Nothing to send.
    if(m_no_body || piece.empty()) return;

    if(m_chunked) *m_s << chunk(piece) << std::flush;
    else          *m_s << piece << std::flush;
}

void server::responder::end() {
    if(!m_started || m_ended) return;

    m_ended = true;

    if(m_chunked) *m_s << last_chunk() << std::flush;
}

bool server::responder::live() const { return m_s && bool(*m_s); }

/**
 * The request target as a path, or a reason it is not one.
 *
 * Shared by the blocking and the suspending serve, because a difference
 * between them here is a *routing* difference -- one server reaching a handler
 * the other refuses -- and that is the last place two implementations should
 * be allowed to drift.
 *
 * @return whether it decoded; `why` carries the 400's body if not
 */
bool server::path_of(const std::string& target, std::string& path,
                     std::string& why)
{
    // An encoded separator is refused rather than decoded: decoding one would
    // change how many segments the path has, which is the shape of a traversal
    // bug.  Everything else is decoded, because RFC 3986 2.1 makes "%65" and
    // "e" the same character.
    const std::string lowered = util::http::fold(target);

    if(lowered.find("%2f") != std::string::npos ||
       lowered.find("%5c") != std::string::npos) {
        why = "an encoded path separator in the request target\n";

        return false;
    }

    try {
        util::URL u;

        u.parse_reference(target);

        path = util::uri::decode(u.get_path());
    }
    catch(std::exception&) {
        why = "not a request target\n";

        return false;
    }

    return true;
}

bool server::params::has(const std::string& name) const {
    for(std::size_t i = 0; i < m_named.size(); i++) {
        if(m_named[i].first == name) return true;
    }

    return false;
}

std::string server::params::get(const std::string& name) const {
    for(std::size_t i = 0; i < m_named.size(); i++) {
        if(m_named[i].first == name) return m_named[i].second;
    }

    return std::string();
}

std::vector<std::string> server::split_path(const std::string& path) {
    std::vector<std::string> out;
    std::string seg;

    // Empty segments are dropped, so "/a//b/" and "/a/b" are the same route.
    // That is a choice rather than an obligation -- RFC 3986 says the two are
    // different resources -- and it is made because the alternative is a table
    // where a trailing slash silently 404s.
    for(std::size_t i = 0; i <= path.size(); i++) {
        if(i == path.size() || path[i] == '/') {
            if(!seg.empty()) out.push_back(seg);

            seg.clear();
        }
        else {
            seg += path[i];
        }
    }

    return out;
}

void server::compile_route(entry& e) {
    const std::vector<std::string> parts = split_path(e.path);

    for(std::size_t i = 0; i < parts.size(); i++) {
        const std::string& s = parts[i];

        segment seg;

        if(s == "*") {
            // Only last, because anything after it could never be reached --
            // and a pattern with unreachable parts is a mistake worth refusing
            // at registration, where the caller is, rather than at match time.
            if(i + 1 != parts.size())
                throw error("a route pattern has * before its end: \"" +
                            e.path + "\"");

            seg.what = segment::rest;
            e.wild = true;
        }
        else if(s.size() > 1 && s.front() == '{' && s.back() == '}') {
            seg.what = segment::named;
            seg.text = s.substr(1, s.size() - 2);

            if(seg.text.empty())
                throw error("a route pattern has an unnamed parameter: \"" +
                            e.path + "\"");
        }
        else {
            // A stray brace is a typo that would otherwise route nothing and
            // say nothing about why.
            if(s.find('{') != std::string::npos ||
               s.find('}') != std::string::npos) {
                throw error("a route pattern has a brace in a literal "
                            "segment: \"" + e.path + "\"");
            }

            seg.what = segment::literal;
            seg.text = s;

            e.literals++;
        }

        e.segs.push_back(seg);
    }
}

bool server::matches(const entry& e, const std::vector<std::string>& parts,
                     params& into)
{
    params got;

    for(std::size_t i = 0; i < e.segs.size(); i++) {
        const segment& seg = e.segs[i];

        if(seg.what == segment::rest) {
            // Everything left, joined back up.  Zero segments is a match: an
            // application serving /static/* should answer /static the way it
            // answers /static/, and refusing here would make that a 404 the
            // caller cannot see the reason for.
            std::string rest;

            for(std::size_t j = i; j < parts.size(); j++) {
                if(!rest.empty()) rest += "/";

                rest += parts[j];
            }

            got.m_rest = rest;

            into = got;

            return true;
        }

        if(i >= parts.size()) return false;

        if(seg.what == segment::literal) {
            if(seg.text != parts[i]) return false;
        }
        else {
            got.m_named.push_back(std::make_pair(seg.text, parts[i]));
        }
    }

    // A pattern with no * must account for every segment, or /a would match
    // /a/b and a prefix would have been registered by accident.
    if(parts.size() != e.segs.size()) return false;

    into = got;

    return true;
}

const server::entry* server::route_for(const std::string& method,
                                       const std::string& path,
                                       params& into,
                                       std::vector<std::string>& allowed) const
{
    const std::vector<std::string> parts = split_path(path);

    const entry* best = 0;
    params best_params;

    for(const entry& e : m_routes) {
        params got;

        if(!matches(e, parts, got)) continue;

        if(e.method != method) {
            // The path exists, for something else.  Collected rather than
            // returned, because a later route may still match the method --
            // and because Allow is a list.
            bool seen = false;

            for(std::size_t i = 0; i < allowed.size(); i++) {
                if(allowed[i] == e.method) seen = true;
            }

            if(!seen) allowed.push_back(e.method);

            continue;
        }

        // Most literal segments wins; first registered breaks a tie, which is
        // what the old exact-match scan did for the only case it had.
        if(best == 0 || e.literals > best->literals) {
            best = &e;
            best_params = got;
        }
    }

    // **A HEAD is a GET that stops at the headers** -- RFC 9110 9.3.2 -- so a
    // table with a GET route and no HEAD one answers HEAD with it.  A route
    // registered for HEAD explicitly still wins, because it was found above.
    if(best == 0 && method == "HEAD") {
        for(const entry& e : m_routes) {
            params got;

            if(e.method != "GET" || !matches(e, parts, got)) continue;

            if(best == 0 || e.literals > best->literals) {
                best = &e;
                best_params = got;
            }
        }
    }

    // And Allow says HEAD wherever it says GET, for the same reason: a client
    // told the path takes GET and not HEAD would be told something untrue.
    for(std::size_t i = 0; i < allowed.size(); i++) {
        if(allowed[i] == "GET") {
            allowed.push_back("HEAD");
            break;
        }
    }

    if(best != 0) into = best_params;

    // `allowed` is deliberately *not* cleared here.  It is meaningful only
    // when this returns null -- that is the contract, and it is written on the
    // declaration -- and clearing it would be a guard with nothing behind it:
    // every caller tests the return first, so a stale list cannot be read.
    // Tried as a break; nothing failed, which is what dead code looks like.
    return best;
}

/**
 * 405, with the Allow a client is owed.
 *
 * RFC 9110 15.5.6 requires Allow on a 405 -- a bare one tells a client the
 * path exists and nothing about what to do instead, which is worse than the
 * 404 it replaces.
 */
static void method_not_allowed(server::response& r,
                               const std::string& method,
                               const std::vector<std::string>& allowed)
{
    std::string list;

    for(std::size_t i = 0; i < allowed.size(); i++) {
        if(!list.empty()) list += ", ";

        list += allowed[i];
    }

    r.status(405).field("Allow", list).type("text/plain")
     .body("that path does not take " + method + "; it takes " + list + "\n");
}

void server::serve(sys::socketstream& s, const sys::peer&) {
    response r;

    Request q;

    try {
        q = util::http::read_request_head(s, m_options.max_head);

        // RFC 9110 10.1.1.  curl sends this for any POST over about a kilobyte
        // and then waits for it; a server that ignores it makes every such
        // client wait out its own timeout before sending the body.
        if(util::http::fold(q.fields().get("Expect")) == "100-continue") {
            s << "HTTP/1.1 100 Continue\r\n\r\n" << std::flush;
        }

        q.set_body(util::http::read_body(s, q.body_framing(), q.content_length(),
                                         m_options.max_body));
    }
    catch(util::http::error& e) {
        // A message this cannot read at all.  Answer, so a client learns
        // something rather than seeing a bare close, and stop.
        r.status(400).type("text/plain").body(std::string(e.what()) + "\n");

        s << r.str(m_options.server_name) << std::flush;

        return;
    }

    // The path, without the query.  parse_reference is what learned to read one
    // of these two branches ago.
    std::string path;
    std::string why;

    if(!path_of(q.target(), path, why)) {
        r.status(400).type("text/plain").body(why);

        s << r.str(m_options.server_name) << std::flush;

        return;
    }

    params captured;
    std::vector<std::string> allowed;

    const entry* chosen = route_for(q.method(), path, captured, allowed);

    // The path is known, for other methods.  405 rather than 404, because the
    // two say different things and a client can act on only one of them.
    if(chosen == 0 && !allowed.empty()) {
        response r;

        method_not_allowed(r, q.method(), allowed);

        s << r.str(m_options.server_name) << std::flush;

        return;
    }

    // A streaming route takes a different path entirely, because the promise
    // the buffered one makes -- that a handler which throws is still answered
    // -- cannot be kept once bytes have gone.
    // Registered for an async server, reached on a blocking one.  Refused
    // where it is reached, so one route table can be built for both modes.
    // **Only when there is no blocking one beside it.**  A route may carry a
    // handler of each kind -- server::files registers both, so one
    // registration serves either server -- and refusing because the other kind
    // exists would make carrying both useless.
    if(chosen && (chosen->async_stream || chosen->async_param_stream) &&
       !(chosen->stream || chosen->param_stream)) {
        response oops;

        oops.status(500).type("text/plain")
            .body("that route has a suspending handler and this server is "
                  "blocking\n");

        s << oops.str(m_options.server_name) << std::flush;

        return;
    }

    const bool head_only = q.method() == "HEAD";

    if(chosen && (chosen->stream || chosen->param_stream)) {
        responder out(s, m_options.server_name);

        if(head_only) out.suppress_body();

        // Chunked for an HTTP/1.1 client, the close for anything older.  This
        // server closes after one response either way, so what chunked buys
        // here is only the terminator -- and that is not nothing: without it a
        // client cannot tell a body that ended from a server that died.
        out.framing(q.version() == "HTTP/1.1");

        try {
            if(chosen->param_stream) chosen->param_stream(q, captured, out);
            else                     chosen->stream(q, out);
        }
        catch(...) {
            if(!out.started()) {
                response oops;

                oops.status(500).type("text/plain").body("internal error\n");

                s << oops.str(m_options.server_name) << std::flush;
            }

            // Otherwise there is nothing to say: the status went out long ago
            // and the client sees the close as a truncated body.  Rethrown
            // either way, so sys::server::on_error still learns of it.
            throw;
        }

        // A handler that produced nothing at all is a bug in the handler, and
        // a bare close would look like a crash.
        if(!out.started()) {
            response oops;

            oops.status(500).type("text/plain")
                .body("the handler produced no response\n");

            s << oops.str(m_options.server_name) << std::flush;
        }
        else {
            // **Only on this path.**  A handler that threw took the catch
            // above and was rethrown out of this function, so it never reaches
            // here and its body is never terminated -- which is exactly what
            // tells the client the response is unfinished.
            out.end();
        }

        return;
    }

    // Nothing has reached the socket yet, so a handler that throws can still be
    // answered -- which is the whole reason a response is accumulated rather
    // than streamed.
    //
    // Serialising it is inside the try as well, and that is not fussiness:
    // str() refuses a field a handler built that cannot be sent, and without
    // this the client would get a bare close, which is indistinguishable from
    // a crash.  A refused field is the server's fault and 500 is what says so.
    std::string answer;

    try {
        if(chosen && chosen->param_run) chosen->param_run(q, captured, r);
        else (chosen ? chosen->run : m_otherwise)(q, r);

        // **After the handler, because only then is there anything to
        // compare.**  A handler sets ETag or Last-Modified describing what it
        // produced; whether the client already has that is a question about
        // the pair, and nothing the handler should have to ask.
        //
        // Only for a safe method and only for a 200: a 404 with an ETag is not
        // a thing a client is holding, and turning a POST's answer into a 304
        // would be inventing a meaning RFC 9110 13.1 does not give it.
        if((q.method() == "GET" || q.method() == "HEAD") &&
           r.status() == 200 && not_modified(q, r)) {
            make_not_modified(r);
        }

        // After the conditional, because 13.1 orders them that way: a client
        // whose copy is current wants a 304 rather than a piece of what it
        // already has.
        if(q.method() == "GET" || q.method() == "HEAD") apply_range(q, r);

        answer = head_only ? r.without_body(m_options.server_name)
                           : r.str(m_options.server_name);
    }
    catch(...) {
        response oops;

        oops.status(500).type("text/plain").body("internal error\n");

        s << oops.str(m_options.server_name) << std::flush;

        // Rethrown, so sys::server::on_error sees it: answering the client is
        // not the same as the failure having been dealt with.
        throw;
    }

    s << answer << std::flush;
}


// ---------------------------------------------------------- the async server

sys::task<void> server::async_responder::send(const response& r) {
    if(m_started)
        throw error("a responder sent a whole response after it had begun "
                    "streaming one");

    m_started = true;

    // **A whole response carries its own length, so it can be followed.**
    // This used to serialise with str()'s default, which is close -- so every
    // route that could stream but answered whole shut the connection, and
    // jserve's non-streaming completions did exactly that.  Found by pointing
    // curl at two of them and watching the second say close.
    m_persist = m_want_persist;

    co_await m_writer->write(m_no_body ? r.without_body(m_name, m_persist)
                                       : r.str(m_name, m_persist));
}

sys::task<void> server::async_responder::send_serialised(const std::string& wire) {
    if(m_started)
        throw error("a responder sent a whole response after it had begun "
                    "streaming one");

    m_started = true;

    co_await m_writer->write(wire);
}

void server::async_responder::framing(bool chunked, bool persist) {
    if(m_started)
        throw error("a responder was told how to frame a response it has "
                    "already begun");

    m_chunked = chunked;

    // Recorded, not resolved.  Whether the connection can carry another
    // request depends on how *this* response turns out to be framed, and that
    // is not known until the handler chooses between begin() and send():
    // a streamed body needs chunked to have an end, a whole one has a
    // Content-Length and needs nothing.
    m_want_persist = persist;
}

sys::task<void> server::async_responder::begin(const response& head) {
    if(m_started) throw error("a responder began a response twice");

    m_started = true;
    m_begun = true;

    // See responder::begin: a length the handler already knows beats chunking.
    const bool counted = head.fields().has("Content-Length");

    if(counted) m_chunked = false;

    // Only now: a streamed body is followable exactly when it has an end, and
    // it has one either from the chunked terminator or from a Content-Length.
    // framing() deliberately does not decide this.
    m_persist = m_want_persist && (m_chunked || counted);

    co_await m_writer->write(head.head(m_name, m_chunked, m_persist));

    // See responder::begin: a HEAD gets the head and stops there.
    if(m_no_body) m_ended = true;
}

sys::task<void> server::async_responder::write(const std::string& piece) {
    if(!m_started)
        throw error("a responder wrote a body piece before begin()");

    // A zero-length chunk is the terminator; see the note on the declaration.
    if(m_no_body || piece.empty()) co_return;

    co_await m_writer->write(m_chunked ? chunk(piece) : piece);
}

sys::task<void> server::async_responder::end() {
    // **begun(), not started().**  send() also marks a responder started, and
    // a whole response has already ended -- terminating it again would put a
    // zero-length chunk after a body with a Content-Length, which the next
    // response on the connection would then be read as starting inside.
    //
    // That was live and invisible: the same bug that made send() say close
    // also guaranteed nothing came after the stray chunk to be corrupted by it.
    if(!m_begun || m_ended) co_return;

    m_ended = true;

    if(m_chunked) co_await m_writer->write(last_chunk());
}

server::server(async_t, unsigned short port, const std::string& host,
               const sys::tls_context& tls, const sys::server::policy& p,
               const options& o)
    : m_options(o),
      m_tls(!tls.empty()),
      m_async(true),
      m_request_timeout(p.io_timeout)
{
    m_otherwise = [](const Request&, response& r) {
        r.status(404).type("text/plain").body("not found\n");
    };

    m_transport.reset(new sys::server(
        sys::listener(port, host),
        sys::server::async_handler(
            [this](sys::server::connection& c, const sys::peer& from)
                -> sys::task<void> {
                co_await serve_async(c, from);
            }),
        tls, p));
}

void server::route(const std::string& method, const std::string& path,
                   async_stream_handler h)
{
    entry e;

    e.method = method;
    e.path = path;
    e.async_stream = std::move(h);

    compile_route(e);

    m_routes.push_back(std::move(e));
}

void server::route(const std::string& method, const std::string& path,
                   param_handler h)
{
    entry e;

    e.method = method;
    e.path = path;
    e.param_run = std::move(h);

    compile_route(e);

    m_routes.push_back(std::move(e));
}

void server::route(const std::string& method, const std::string& path,
                   param_stream_handler h)
{
    entry e;

    e.method = method;
    e.path = path;
    e.param_stream = std::move(h);

    compile_route(e);

    m_routes.push_back(std::move(e));
}

void server::route(const std::string& method, const std::string& path,
                   async_param_stream_handler h)
{
    entry e;

    e.method = method;
    e.path = path;
    e.async_param_stream = std::move(h);

    compile_route(e);

    m_routes.push_back(std::move(e));
}

namespace {

    /**
     * The request deadline, disarmed however the request ends.
     *
     * cancel() is called explicitly partway through, because a deadline on
     * *reading the request* must not still be running while a slow handler
     * produces the answer.  That call was here before this and was sufficient,
     * because every path that got past the read reached it.
     *
     * Keep-alive adds paths that do not.  A connection can now end *inside*
     * the read -- a client saying goodbye between requests -- and that is the
     * ordinary way a persistent connection finishes rather than an edge case.
     * A timer left behind there sits in the reactor holding a copy of the
     * token for the rest of its bound, once for every connection a client ever
     * walked away from.
     *
     * Hence a destructor, and an idempotent cancel() for the path that still
     * wants to disarm early.  Between them no timer outlives the request that
     * armed it on any path the reactor's thread takes, which is what keeps
     * sys::deadline's own warning -- that an uncancelled timer fires "into the
     * *next* operation on the same token" -- unreachable on a connection that
     * now *has* a next operation.
     *
     * The exception, and it is not a small one, is in cancel() below: a
     * destructor can run on a thread that must not touch the reactor, and
     * there the timer is abandoned rather than cancelled.
     */
    class armed_deadline {
    public:
        explicit armed_deadline(sys::reactor& r) : m_reactor(&r) {}

        ~armed_deadline() { cancel(); }

        armed_deadline(const armed_deadline&) = delete;
        armed_deadline& operator=(const armed_deadline&) = delete;

        /** Replaces whatever was armed before; seconds <= 0 arms nothing. */
        void arm(double seconds, sys::cancel_token t) {
            cancel();

            if(seconds > 0) {
                m_timer = sys::deadline(
                    *m_reactor,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::duration<double>(seconds)),
                    t);
            }
        }

        void cancel() {
            if(m_timer == sys::reactor::timer_token::none) return;

            // **Only ever from the reactor's own thread**, and this guard is
            // the whole reason a destructor here is safe at all.
            //
            // A destructor runs wherever the frame is destroyed, and one of
            // the paths that destroys this frame is a cancellation: stop()
            // ends a parked connection by requesting its token, and it does
            // that on the *calling* thread, so the coroutine unwinds there
            // while the reactor thread is still running a pass.
            // reactor::cancel() mutates two containers with no lock, so
            // cancelling from that unwind is a data race -- and the symptom is
            // heap corruption a long way from the cause, which is how this was
            // found rather than reasoned about.
            //
            // Abandoning the timer instead is safe and cheap: it fires later
            // into a token that has already been requested, and requesting a
            // requested token does nothing.  That is also exactly what the
            // code before this class did on the same path, by not cancelling
            // at all.
            if(m_reactor->on_reactor_thread()) m_reactor->cancel(m_timer);

            m_timer = sys::reactor::timer_token::none;
        }

    private:
        sys::reactor*             m_reactor;
        sys::reactor::timer_token m_timer = sys::reactor::timer_token::none;
    };

}

sys::task<void> server::serve_async(sys::server::connection& c,
                                    const sys::peer& from)
{
    // **Keep-alive, and why the blocking server does not get it.**
    //
    // A persistent connection spends most of its life idle, waiting for a
    // request that may never come.  In serve() that idle time would be a
    // *thread*: a handful of clients holding connections open would occupy
    // every worker in the pool and the server would stop accepting -- so
    // keep-alive there is a denial of service a server performs on itself.
    // Here an idle connection is a suspended coroutine and a registration in
    // the reactor.  It holds a descriptor and no thread.
    //
    // So this is something the asynchronous server can do and the blocking one
    // cannot, rather than a feature missing from the blocking one.  It is also
    // the first thing in this tree that is *only* worth having because of #4.
    std::size_t served = 0;

    while(co_await serve_request_async(c, from, served)) {
        ++served;
    }
}

sys::task<bool> server::serve_request_async(sys::server::connection& c,
                                            const sys::peer&,
                                            std::size_t served)
{
    async_responder out(c.writer(), m_options.server_name, c.reactor(),
                        c.pool());

    // **The slow-loris bound.**  Every piece of this existed before and
    // nothing armed it: the connection is a coroutine carrying a token, the
    // token now ends a wait rather than merely marking it (#212), and a
    // deadline is a timer that requests one.  This is what uses them.
    //
    // Over the whole request read, not per operation -- which is the
    // difference from the blocking server's SO_RCVTIMEO, and is what makes a
    // client sending one octet every twenty-nine seconds a dropped connection
    // here and an indefinite one there.
    //
    // **Waiting for a request to start is a different question from reading
    // one, and they get different answers.**  Three of them:
    //
    //   a new connection, silent           initial_idle_timeout   5s
    //   a request, once it has started     io_timeout            30s
    //   a reused connection, between       idle_timeout          60s
    //
    // The middle one is the only one that is really a *request* timeout; the
    // other two bound silence at two points where silence means different
    // things.  A client that has just connected is about to speak, so five
    // seconds is generous.  A client that polls every fifteen seconds has a
    // perfectly good reason to be quiet, and a five second bound there would
    // hand it a new connection every time and make keep-alive an elaborate way
    // of changing nothing.
    //
    // Collapsing any two of these means one of them is wrong: an earlier draft
    // used a single timer per request and so applied the *idle* bound to the
    // whole of every request after the first, which gave a large second POST
    // five seconds where the first got thirty.
    armed_deadline limit(c.reactor());

    Request q;

    // **co_await cannot appear in a catch handler.**  That is a language rule,
    // not a limitation of anything here, and it shapes every error path below:
    // the failure is recorded, the catch ends, and the answer is written
    // afterwards.  The blocking serve() writes from inside its catch, which
    // reads more directly and is not available here.
    std::string bad;

    try {
        // Phase one: nothing has arrived yet, so this is silence rather than a
        // slow request, and which silence it is depends on whether this
        // connection has ever been used.
        //
        // fill() rather than sys::readable() on the descriptor, because for a
        // TLS connection those are **different questions** -- the socket can be
        // ready with a record that yields no plaintext, and plaintext can be
        // waiting with the socket quiet.  async_tls answers the one that
        // matters and readable() would answer the other.
        if(c.reader().buffered() == 0) {
            armed_deadline idle(c.reactor());

            idle.arm(served == 0 ? m_options.initial_idle_timeout
                                 : m_options.idle_timeout,
                     c.token());

            // The answer is deliberately dropped.  read_head_if_any below is
            // the single place that decides what an ended connection means,
            // and two places deciding that is how they come to disagree.
            (void) co_await c.reader().fill();
        }

        // Phase two: something is here, so from now on this is a request being
        // read and the request bound applies -- the same one the first request
        // on the connection got, reset per request the way a fresh connection
        // would have got it.
        limit.arm(m_request_timeout, c.token());

        // **The polite goodbye**, which a closing server never had to know
        // about: a peer that shuts the connection between requests has not
        // sent a broken message, it has finished.  read_head would call that
        // a head that ended after zero octets and this would answer 400 down
        // a socket that is already gone.
        std::string head;

        if(!co_await util::http::read_head_if_any(c.reader(),
                                                  m_options.max_head, head)) {
            co_return false;
        }

        // Read on the reactor's thread, because that is where the descriptor
        // is.  **Parsed on a worker**, because it is not: parsing a head of
        // eight fields measures at 59us, and every microsecond of it is a
        // microsecond no other connection is being dispatched.  The hop costs
        // 5.85us round trip, so it pays for itself at about one field.
        //
        // read_head and parse_request_head were already separate -- the
        // RFC-grammar work split framing from parsing for its own reasons --
        // so this is a hop between two calls that already existed.
        co_await sys::on_pool(c.pool());

        q = util::http::parse_request_head(head);

        co_await sys::on_reactor(c.reactor());

        // RFC 9110 10.1.1.  curl sends this for any POST over about a
        // kilobyte and then waits for it; a server that ignores it makes
        // every such client wait out its own timeout before sending the body.
        if(util::http::fold(q.fields().get("Expect")) == "100-continue")
            co_await c.writer().write("HTTP/1.1 100 Continue\r\n\r\n");

        // **Read whole, which is also what leaves the connection at a message
        // boundary.**  A body this did not consume would still be in the
        // reader, and the next read_head would parse the client's leftover
        // octets as a request line -- which is the smuggle, arriving by way
        // of a server that simply forgot to finish reading.
        q.set_body(co_await util::http::read_body(c.reader(), q.body_framing(),
                                                  q.content_length(),
                                                  m_options.max_body));
    }
    catch(util::http::error& e) {
        // A message this cannot read at all.  Answer, so a client learns
        // something rather than seeing a bare close, and stop.
        bad = std::string(e.what()) + "\n";
    }
    catch(sys::cancelled&) {
        // The deadline, or a shutdown.  Nothing is sent: the client has not
        // finished asking, and a 408 down a connection whose peer is still
        // mid-request is as likely to be missed as read.  The connection
        // closes when this returns.
        co_return false;
    }

    // In hand.  A handler that takes a long time to answer is a different
    // question, and leaving this armed would turn a slow reply into a dropped
    // connection.
    limit.cancel();

    // **Every 400 closes, and the framing ones have to.**  Where this message
    // ended is exactly what failed, so where the next one starts is not
    // something this can claim to know either -- reading on would mean taking
    // whatever the client put there as a request, which is the smuggle.
    //
    // A bad request-target is a well-framed message and could be answered on a
    // connection that continues.  It is not, because one rule is easier to be
    // sure of than two, and a client sending one is not in a conversation
    // worth the saved handshake.
    if(!bad.empty()) {
        response r;

        r.status(400).type("text/plain").body(bad);

        co_await out.send(r);

        co_return false;
    }

    std::string path;

    if(!path_of(q.target(), path, bad)) {
        response r;

        r.status(400).type("text/plain").body(bad);

        co_await out.send(r);

        co_return false;
    }

    params captured;
    std::vector<std::string> allowed;

    const entry* chosen = route_for(q.method(), path, captured, allowed);

    if(chosen == 0 && !allowed.empty()) {
        response r;

        method_not_allowed(r, q.method(), allowed);

        co_await out.send(r);

        co_return false;
    }

    // A route registered for the blocking streaming handler cannot run here:
    // responder writes to a socketstream and there is not one.  Refused where
    // it is reached rather than where it was registered, so one route table
    // can be built for both modes.
    // As above, the other way round: refused only when there is no suspending
    // handler on the same route.
    if(chosen && (chosen->stream || chosen->param_stream) &&
       !(chosen->async_stream || chosen->async_param_stream)) {
        response oops;

        oops.status(500).type("text/plain")
            .body("that route has a blocking streaming handler and this "
                  "server is asynchronous\n");

        co_await out.send(oops);

        co_return false;
    }

    if(chosen && (chosen->async_stream || chosen->async_param_stream)) {
        // Chunked needs an HTTP/1.1 client; anything older gets the close, and
        // gets it as the framing rather than as an afterthought.
        const bool can_chunk = q.version() == "HTTP/1.1";

        // Note what this does *not* ask: whether the body can be framed so
        // that something may follow it.  framing() is the one place that
        // reconciles the two, and out.persist() below is its answer -- so
        // there is a single place to be wrong rather than two that must agree.
        const bool persist = m_options.keep_alive &&
                             util::http::persistent(q) &&
                             served + 1 < m_options.max_requests;

        out.framing(can_chunk, persist);

        if(q.method() == "HEAD") out.suppress_body();

        // Held rather than rethrown with a bare `throw;` outside the catch:
        // by then there is no exception in flight and a bare throw calls
        // std::terminate.  That is the sharp edge the co_await-in-a-catch rule
        // creates, and it is a crash rather than a diagnostic.
        std::exception_ptr threw;

        try {
            if(chosen->async_param_stream)
                co_await chosen->async_param_stream(q, captured, out);
            else
                co_await chosen->async_stream(q, out);
        }
        catch(...) {
            threw = std::current_exception();
        }

        // **Snapshot before anything below sends a 500**, because send() marks
        // the responder started too -- and a 500 is a whole Content-Length
        // response that says close, not a streamed body that can be continued
        // from.  Conflating the two would keep a connection open after telling
        // the client it was closing.
        const bool streamed = out.started();

        if(threw && !out.started()) {
            response oops;

            oops.status(500).type("text/plain").body("internal error\n");

            co_await out.send(oops);
        }

        // A handler that produced nothing at all is a bug in the handler, and
        // a bare close would look like a crash.
        if(!threw && !out.started()) {
            response oops;

            oops.status(500).type("text/plain")
                .body("the handler produced no response\n");

            co_await out.send(oops);
        }

        // Terminated only when the handler returned.  A handler that failed
        // leaves the body unfinished on purpose: the status left long ago and
        // cannot be taken back, so the one thing still available to say "this
        // is not the whole response" is to not write the terminator.
        if(!threw && streamed) co_await out.end();

        // Rethrown so sys::server::on_error still learns of it.  Once begin()
        // has gone out there is nothing to *say* -- which is what responder
        // gave up in #200 and is no different here.
        if(threw) std::rethrow_exception(threw);

        // And now a streaming response can be followed by another request,
        // which is what the terminating chunk bought.  Asked of the responder
        // rather than recomputed: it is what actually went out on the wire.
        co_return streamed && out.persist();
    }

    // The buffered case, and the common one.  **The handler is not a
    // coroutine**: it fills a response and cannot suspend, so the same handler
    // runs on both servers -- which is what lets the two be compared on one
    // route table.
    // **The handler and the serialisation, on a worker.**
    //
    // The parsing and serialising are the *floor*, and they are measurable:
    // a head of eight fields parses in 59us and a response of ten fields
    // serialises in 52us, because str() checks every field against the
    // grammar on the way out.  That is 111us on the reactor's thread for a
    // handler that does nothing at all, against 5.85us for a hop round trip.
    //
    // **But the handler is the reason.**  A buffered handler is arbitrary
    // code: a file server reads a file, an application server queries
    // something, a template gets rendered.  That is milliseconds, not
    // microseconds -- and without this hop every one of them runs on the one
    // thread that dispatches every other connection, so a single handler
    // reading a slow disk stops the whole server for as long as it takes.
    //
    // The microseconds justify the hop for a trivial handler.  The
    // milliseconds are why it matters.
    //
    // A *streaming* handler is not hopped, above: it does its own writing, so
    // it has to be where the writer is, and a handler that wants the pool for
    // part of its work can co_await on_pool itself.
    //
    // With policy::threads == 0 the queue runs a job on the thread that posted
    // it, so all of this is inline and a serial server pays nothing.
    co_await sys::on_pool(c.pool());

    response r;

    std::exception_ptr threw;

    try {
        if(chosen && chosen->param_run) chosen->param_run(q, captured, r);
        else (chosen ? chosen->run : m_otherwise)(q, r);

        // **After the handler, because only then is there anything to
        // compare.**  A handler sets ETag or Last-Modified describing what it
        // produced; whether the client already has that is a question about
        // the pair, and nothing the handler should have to ask.
        //
        // Only for a safe method and only for a 200: a 404 with an ETag is not
        // a thing a client is holding, and turning a POST's answer into a 304
        // would be inventing a meaning RFC 9110 13.1 does not give it.
        if((q.method() == "GET" || q.method() == "HEAD") &&
           r.status() == 200 && not_modified(q, r)) {
            make_not_modified(r);
        }

        // After the conditional, because 13.1 orders them that way: a client
        // whose copy is current wants a 304 rather than a piece of what it
        // already has.
        if(q.method() == "GET" || q.method() == "HEAD") apply_range(q, r);
    }
    catch(...) {
        threw = std::current_exception();
    }

    // **Decided from the request, then offered to the handler.**  The request
    // says whether the client is willing; max_requests says whether this
    // connection has had its share; and a handler that set its own
    // `Connection: close` overrides both, because it may know something about
    // what it just sent that this does not.
    //
    // The decision has to be made here rather than after the write, because it
    // is a field in the very response being serialised: a server that closed a
    // connection it had just told the client to keep is worse than one that
    // never offered.
    bool persist = m_options.keep_alive &&
                   util::http::persistent(q) &&
                   served + 1 < m_options.max_requests;

    if(persist && util::http::connection_close(r.fields())) persist = false;

    // Serialised here, on the worker, rather than as the argument to write()
    // on the reactor -- which is what it was, and is the half of this that is
    // easy to miss.
    std::string wire;

    if(!threw) {
        wire = q.method() == "HEAD"
               ? r.without_body(m_options.server_name, persist)
               : r.str(m_options.server_name, persist);
    }

    co_await sys::on_reactor(c.reactor());

    if(threw) {
        response oops;

        oops.status(500).type("text/plain").body("internal error\n");

        co_await out.send(oops);

        std::rethrow_exception(threw);
    }

    co_await out.send_serialised(wire);

    co_return persist;
}

}
}
}
