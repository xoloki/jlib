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

/**
 * juanaco -- jcode with a transcript that stays put.
 *
 * A single pane: the task summarised at the top, a scrolling transcript in the
 * middle, a prompt pinned below it, and a status line under that.
 *
 * **A reply now comes from somewhere** (#447, phase 3 of #444). The whole of
 * jcode runs on a thread of its own and reports back through a pipe, so the
 * prompt keeps taking keys and the screen keeps repainting through a request
 * that blocks for minutes. Still no streaming -- the reply arrives as one
 * body, which is what keeps the relay's output to a handful of events while
 * the loop is being proven -- and still no confirm dialog. #448 has the
 * first, #449 the second.
 *
 * All of it is driven by `make check` against the scripted endpoint
 * `app_jcode_run_test` already hand-rolls: no GGUF, no model, no network.
 * That combination -- a scripted server and a pty -- is what makes a curses
 * harness testable at all, and it is why phase 1 came first.
 *
 * ### Why this is a second program rather than a flag on jcode
 *
 * jcode is the tested one -- `app_jcode_test` covers the pieces and
 * `app_jcode_run_test` drives the whole program against a scripted endpoint
 * and asserts what lands on disk -- and both do it down a pipe. A curses
 * program cannot be driven that way without a pty, which is why #445 came
 * first. Folding them together would mean giving up those tests or carrying
 * two output paths through every function.
 *
 * jalpaca stands in the same relation to jchat, for the same reason, and its
 * header makes the argument at more length.
 *
 * ### The transcript is a model, not a screen
 *
 * jalpaca keeps its transcript in a curses pad, and its own comment says what
 * that costs: the contents are wrapped to a width and "cannot be rewrapped
 * without re-rendering everything -- which would need the transcript kept as
 * text as well".
 *
 * So this keeps the text. `transcript` is a list of typed entries and the pad
 * is built from them, which is what makes a resize re-wrap rather than
 * re-flow badly, and what will let `/files` show the same conversation a
 * different way without a second copy of it.
 */

#include <jlib/apps/jcode.hh>

#include <jlib/ai/openai.hh>
#include <jlib/net/http.hh>
#include <jlib/sys/relay.hh>
#include <jlib/util/URL.hh>
#include <jlib/util/utf8.hh>

// **Last, and that is not tidiness.** <ncurses.h> defines `timeout`, `erase`,
// `move`, `refresh` and a dozen more as macros, so a jlib header included
// after it gets its own identifiers rewritten.
#include <ncurses.h>

#include <clocale>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace ai = jlib::ai;
namespace oa = jlib::ai::openai;
namespace http = jlib::net::http;
namespace jcode = jlib::apps::jcode;
namespace sys = jlib::sys;
namespace util = jlib::util;

namespace {

/**
 * The first `chars` characters of a UTF-8 string, not the first bytes.
 *
 * `util/utf8.hh` validates and decodes incrementally but does not truncate,
 * and truncating is what a fixed-width pane needs. A byte starts a character
 * unless it is a continuation, which is `10xxxxxx` -- so counting lead bytes
 * counts characters, and cutting on one cannot split a character in half.
 *
 * Characters, not columns: a CJK character occupies two cells and this counts
 * it as one, so a line of them wraps late. Correcting that needs `wcwidth`
 * and a decode per character, and the narrow version is enough until
 * something in the transcript is wide enough to care.
 */
static std::string head(const std::string& s, std::size_t chars) {
    std::size_t i = 0;
    std::size_t n = 0;

    while(i < s.size() && n < chars) {
        i++;

        while(i < s.size() && (s[i] & 0xc0) == 0x80) i++;

        n++;
    }

    return s.substr(0, i);
}

/** How many characters, for the caret. */
static std::size_t chars_in(const std::string& s, std::size_t upto) {
    std::size_t n = 0;

    for(std::size_t i = 0; i < upto && i < s.size(); i++)
        if((s[i] & 0xc0) != 0x80) n++;

    return n;
}

/** What a transcript entry is, which decides how it is drawn. */
enum class kind {
    you,        ///< what was typed
    model,      ///< what came back
    tool,       ///< a tool call the model asked for
    build,      ///< output from the build
    edit,       ///< an edit applied, refused or left alone
    note        ///< juanaco talking about itself
};

struct entry {
    kind k = kind::note;
    std::string text;
};

/**
 * The conversation, as text rather than as pixels.
 *
 * Rendering happens in `screen`, from this. Keeping both would be two places
 * to be wrong about what was said.
 */
class transcript {
public:
    void add(kind k, const std::string& text) {
        entry e;

        e.k = k;
        e.text = text;

        m_entries.push_back(e);
    }

    const std::vector<entry>& entries() const { return m_entries; }

private:
    std::vector<entry> m_entries;
};

/** What the top line says, and what `/mode` changes. */
struct state {
    std::string task = "(no task yet)";
    std::string model = "(none)";

    /**
     * Which shape to ask the model to answer in.
     *
     * **`whole`, measured rather than assumed.** This read `hunks` until
     * #447 pointed juanaco at a real jserve, and the default path failed on
     * the first trivial edit: Qwen2.5-Coder-7B answered with
     * `path/to/hello.cc` -- the prompt's own example prefix pasted back --
     * and the edit was refused, because **jcode's leniency is whole-only**.
     * `clean()` absorbs bold, backticks, a heading, a trailing colon and
     * that prefix; `hunks_in` takes the line before the marker raw and
     * calls none of it. The same model in `whole` omitted the filename
     * entirely and was rescued.
     *
     * So this matches jcode, whose own reason is that `whole` is what jserve
     * was verified against end to end. `hunks` stays a `/mode` away and is
     * worth having -- #348 exists because the two fail differently -- but a
     * default that cannot survive a one-line edit is not a choice.
     */
    jcode::format edits = jcode::format::whole;

    bool build = false;
    unsigned int rounds = 8;
    unsigned int attempts = 3;

    // Counters for /stats and the status line, totalled across every
    // exchange of the session. Zero until something has been sent, which is
    // a different fact from zero because nothing was counted -- see /stats,
    // which says which it is.
    unsigned long long tokens_in = 0;
    unsigned long long tokens_out = 0;
    unsigned int context_used = 0;
    unsigned int context_total = 0;

    /** Whether a request is in flight.  Owned by the UI thread. */
    bool working = false;

    std::string mode_name() const {
        return edits == jcode::format::whole ? "whole" : "hunks";
    }
};

// ------------------------------------------------------------- the worker

/**
 * What the program was started with, as opposed to what was typed into it.
 *
 * The split from `state` is the split between a flag and a command: a URL is
 * decided once, and the edit format is something `/mode` changes between one
 * request and the next.
 */
struct options {
    std::string url = "http://127.0.0.1:8080/v1";
    std::string model;
    std::string root = ".";

    /**
     * The context to lay out against, in tokens.
     *
     * **Told, not asked**, for the reason jcode's own options record: nothing
     * in the OpenAI protocol says how big a model's context is, and
     * `/v1/models` answers names.
     */
    std::size_t context = 8192;

    unsigned int tokens = 0;        ///< max_tokens; 0 leaves it to the server

    /**
     * Seconds to wait for a reply.  Ten minutes, not thirty seconds.
     *
     * jcode measured this: the first request against a freshly loaded 7B
     * model failed at 30.050 seconds -- `net::http`'s default, to the
     * millisecond -- because a cold model compiles its kernels before it
     * answers. **An inference server is quiet precisely while it is
     * working**, so the usual HTTP reasoning is inverted here.
     *
     * It is also why the worker exists at all. A UI that blocked for this
     * long would be indistinguishable from one that had crashed.
     */
    double timeout = 600;

    /** The build command, split on whitespace.  Empty means none at all. */
    std::string build;

    bool yes = false;

    /** The files put in front of the model, named on the command line. */
    std::vector<std::string> names;
};

/**
 * One thing the worker has to say.
 *
 * ## One stream rather than a channel per sort of news
 *
 * The order *is* the information: the reply, then what it cost, then what was
 * written, is a different story from any other arrangement of those three
 * facts. Two relays would have no order between them and reconstructing one
 * at the far end means timestamps nobody wants.
 *
 * So `news` discriminates a payload rather than selecting a pipe. `say` is
 * prose and `k` decides which row it becomes; `counts` is the one event that
 * is not a line of text.
 */
enum class news { say, counts };

struct event {
    news n = news::say;

    kind k = kind::note;
    std::string text;

    unsigned long long tokens_in = 0;
    unsigned long long tokens_out = 0;
    unsigned int context_used = 0;
    unsigned int context_total = 0;
};

/**
 * Everything the worker reads, **copied rather than referred to**.
 *
 * That copy is the whole of the thread discipline here. The UI thread owns
 * `state` and goes on editing it -- typing `/mode whole` while a request is
 * in flight is an ordinary thing to do -- so a worker holding a reference
 * would be reading a field as it changed. Fixing the job at the moment the
 * request is sent is also what a user means by having set the mode first.
 */
struct job {
    std::string url;
    std::string model;
    std::string root;
    std::string build;      ///< empty when /mode build is off, or none given

    std::size_t context = 8192;
    unsigned int tokens = 0;
    double timeout = 600;

    unsigned int rounds = 8;
    unsigned int attempts = 3;

    bool yes = false;

    jcode::format shape = jcode::format::whole;

    std::string request;
    std::vector<std::string> names;
};

job compose(const options& o, const state& s, const std::string& request) {
    job j;

    j.url = o.url;
    j.model = o.model;
    j.root = o.root;

    // **`/mode build on` with no --build has nothing to turn on.** The build
    // tool is registered only when a command was named (see jcode::toolbox),
    // so the two have to agree here rather than in the builder.
    j.build = s.build ? o.build : std::string();

    j.context = o.context;
    j.tokens = o.tokens;
    j.timeout = o.timeout;

    j.rounds = s.rounds;
    j.attempts = s.attempts;

    j.yes = o.yes;
    j.shape = s.edits;

    j.request = request;
    j.names = o.names;

    return j;
}

bool read_file(const std::string& root, const std::string& name,
               jcode::source& into)
{
    std::ifstream in(root + "/" + name, std::ios::binary);

    if(!in) return false;

    std::ostringstream all;

    all << in.rdbuf();

    into.name = name;
    into.content = all.str();

    return true;
}

/** The first `most` lines of something, for output that has no business
 *  filling a transcript. */
std::vector<std::string> first_lines(const std::string& text, std::size_t most) {
    std::vector<std::string> out;

    std::istringstream in(text);
    std::string line;

    while(out.size() < most && std::getline(in, line)) out.push_back(line);

    return out;
}

/**
 * The whole of jcode, on a thread of its own.
 *
 * ## Why a thread rather than the reactor
 *
 * `net::http` is deliberately synchronous, and CLAUDE.md is explicit that
 * jlib's async machinery exists for things that are *not* a single connection
 * in their own thread. This is a single connection in its own thread. So is
 * the build, and so is the write.
 *
 * `sys::relay` is the established shape for exactly that -- jserve uses it
 * for the same purpose -- and what it gives that a bare `std::thread` does
 * not is a **descriptor**: output arrives with a byte down a pipe, so the
 * curses loop can drain it between keystrokes without a condition variable
 * or a lock it has to remember to take.
 *
 * ## What does not cross the boundary, and the one thing that will have to
 *
 * Items one way, a stop flag the other. **The confirm is the exception and it
 * is not here**: asking before a write means the worker blocking on a
 * keystroke the UI thread owns, which is the one direction `relay` has no
 * answer for. So this phase either writes with `--yes` or announces what it
 * would have written and does nothing, and #449 designs the way back.
 *
 * `stop()` is also weaker than it looks. `wanted()` is read between items,
 * and a worker sitting in a ten-minute HTTP read is not between items. The
 * exchange checks it before each round, which bounds cancellation at a round
 * boundary rather than at a packet -- see the end of main() for what that
 * costs on the way out.
 */
void work(sys::relay<event>& r, const job& j) {
    const auto say = [&r](kind k, const std::string& text) {
        event e;

        e.k = k;
        e.text = text;

        r.emit(e);
    };

    std::vector<jcode::source> files;

    for(const std::string& n : j.names) {
        jcode::source src;

        if(read_file(j.root, n, src)) files.push_back(src);
        else say(kind::note, "cannot read " + j.root + "/" + n);
    }

    const std::vector<jcode::tool> tools = jcode::toolbox(j.root, j.build);

    const jcode::plan plan =
        jcode::lay_out(j.request, files, j.context, tools, j.shape);

    for(const std::string& d : plan.dropped) say(kind::note, "left out " + d);

    say(kind::note, std::to_string(plan.sent.size()) + " of " +
                    std::to_string(files.size()) + " file(s), about " +
                    std::to_string(plan.estimate) + " tokens of " +
                    std::to_string(j.context));

    const util::URL base(j.url);
    const util::URL target(j.url + "/chat/completions");

    http::options net;

    net.timeout = j.timeout;

    // A whole file of source, and the protocol's envelope round it.  The
    // 1 MiB default is sized for a token endpoint's reply.
    net.max_body = 32 << 20;

    // **Made once, outside the lambda.** A harness that opens a connection
    // per call is #221, and this one would do it once a round.
    std::unique_ptr<http::connection> conn;

    std::size_t prompt_tokens = 0;
    bool first = true;

    unsigned long long in = 0;
    unsigned long long out = 0;

    const jcode::exchange once =
        [&](const std::vector<ai::message>& turns,
            const std::string& declared) -> oa::answer
    {
        // The one place cancellation can land, which is between rounds and
        // not inside one; see the class note above.
        if(!r.wanted()) throw std::runtime_error("nobody is listening");

        oa::request req;

        req.model = j.model;

        // **Not streaming, and this is the phase allowed not to be.** One
        // body means the relay carries a handful of events rather than one
        // per token, so the loop is provable before the token path exists.
        // #448 turns it on, and Escape with it.
        req.stream = false;

        req.max_tokens = j.tokens;
        req.tools = declared;
        req.messages = turns;

        if(!conn) conn.reset(new http::connection(base, net));

        util::http::fields send;

        send.add("Content-Type", "application/json");

        const util::http::Response res =
            conn->request("POST", target, send, req.str());

        if(!res.ok()) {
            oa::failure f;

            // Thrown rather than returned: converse turns it into an ending
            // that says the far end refused, which is a different outcome
            // from the model having had nothing to say.
            throw std::runtime_error(
                "the server refused: " +
                (oa::failure::parse(res.body(), f) ? f.message : res.reason()));
        }

        const oa::answer a = oa::answer::parse(res.body());

        if(!a.content.empty()) say(kind::model, a.content);

        in += a.prompt_tokens;
        out += a.completion_tokens;

        // The first reply is the one the estimate was made against.
        if(first) { prompt_tokens = a.prompt_tokens; first = false; }

        event e;

        e.n = news::counts;
        e.tokens_in = in;
        e.tokens_out = out;

        // What this exchange occupied, not the session's total: the context
        // is a window the next request has to fit in, and a running sum of
        // every turn ever sent is not a thing that can overflow it.
        e.context_used = a.prompt_tokens + a.completion_tokens;
        e.context_total = unsigned(j.context);

        r.emit(e);

        return a;
    };

    // **Everything that cannot live in a library**, which for the write is
    // #242's constraint: every write announced, and never a tool the model
    // calls, because that moves the decision inside the loop.
    const jcode::writer put = [&](const jcode::reply& parsed) -> bool {
        for(const std::string& g : parsed.guesses())
            say(kind::note, "guessed: " + g);

        // Decided first, written second, so nothing is announced that then
        // turns out not to apply.  parse()'s own refusals arrive here too:
        // apply() turns each into a result rather than dropping it.
        const std::vector<jcode::result> dry = jcode::apply(parsed, j.root, true);

        jcode::reply going;

        for(const jcode::result& res : dry) {
            if(res.what == jcode::outcome::refused) {
                say(kind::edit, "refused " +
                                (res.name.empty() ? "a block" : res.name) +
                                ": " + res.why);

                continue;
            }

            if(res.what == jcode::outcome::unchanged) {
                say(kind::edit, res.name + " is already what the model sent");

                continue;
            }

            // "would written" is what spell() gives and it is not English.
            const char* verb =
                res.what == jcode::outcome::created ? "create" : "write";

            if(!j.yes) {
                say(kind::edit, std::string("would ") + verb + " " + res.name +
                                " -- nothing is written without --yes until "
                                "#449 adds the confirm");

                continue;
            }

            for(const jcode::edit& e : parsed.edits)
                if(e.name == res.name) going.edits.push_back(e);
        }

        if(going.edits.empty()) return false;

        bool wrote = false;

        for(const jcode::result& res : jcode::apply(going, j.root)) {
            say(kind::edit, std::string(jcode::spell(res.what)) + " " +
                            res.name +
                            (res.why.empty() ? std::string() : ": " + res.why));

            if(res.what == jcode::outcome::written ||
               res.what == jcode::outcome::created) wrote = true;
        }

        return wrote;
    };

    // **Without a build command there is nothing to iterate against**, so one
    // attempt is the whole of it: the builder reports success and the loop
    // stops having written once.
    const jcode::builder check = [&]() -> jcode::built {
        jcode::built b;

        if(j.build.empty()) { b.passed = true; return b; }

        for(const jcode::tool& t : tools) {
            if(t.name != "build") continue;

            b.output = t.run("{}");
            b.passed = b.output.compare(0, 7, "exit 0\n") == 0;

            for(const std::string& l : first_lines(b.output, 12))
                say(kind::build, l);
        }

        return b;
    };

    std::vector<ai::message> opening;

    for(const std::pair<std::string, std::string>& t : plan.turns)
        opening.push_back({ t.first, t.second });

    const jcode::attempts done =
        jcode::until_built(opening, tools, once, put, check, plan.sent,
                           j.attempts, j.rounds);

    for(const jcode::ran& c : done.calls)
        say(kind::tool, c.name +
                        (c.arguments.empty() ? std::string()
                                             : " " + c.arguments) +
                        (c.known ? (c.failed.empty() ? "" : " -- failed: ")
                                 : " -- no such tool") +
                        c.failed);

    // What the guess was worth, said out loud -- from the first exchange,
    // which is the one the estimate was made against.
    const std::string drift =
        jcode::estimate_drift(plan.estimate, prompt_tokens);

    if(!drift.empty()) say(kind::note, drift);

    if(done.why == jcode::settled::built) {
        if(!j.build.empty()) say(kind::note, "and `" + j.build + "` passes");
    }
    else {
        say(kind::note, std::string(jcode::spell(done.why)) +
                        (done.detail.empty() ? std::string()
                                             : " -- " + done.detail));

        // The build's last word, which is why the attempts stopped and what
        // a person needs in order to decide what to do next.
        if(!done.last.empty() && done.why != jcode::settled::nothing)
            for(const std::string& l : first_lines(done.last, 12))
                say(kind::build, l);
    }
}

/**
 * The terminal, and everything drawn on it.
 *
 * `endwin()` runs from the destructor so a throw on the way out of main still
 * puts the terminal back -- jalpaca's rule, and the reason it uses curses at
 * all rather than forty lines of tcsetattr.
 *
 * Called `ui` and not `screen` because ncurses declares `struct screen` in the
 * global namespace. jalpaca gets away with the name by being at file scope,
 * where its class hides the struct; this one is in an anonymous namespace, so
 * from `main` both are visible and the reference is ambiguous.
 */
class ui {
public:
    enum tone { plain = 1, you_tone, note_tone, tool_tone, build_tone, head_tone };

    ui() {
        // Before initscr, and not optional: without it ncurses runs in the C
        // locale and draws each *byte* as a cell, so one four-byte character
        // becomes four cells of rubbish. jalpaca found that with a model that
        // answers in emoji; a coding harness will meet it in a filename.
        std::setlocale(LC_ALL, "");

        initscr();
        cbreak();
        noecho();

        // **nonl, and it is load-bearing.** curses translates Return to
        // newline on input by default, so Enter and Ctrl-J both arrive as 10
        // and nothing can tell them apart -- which makes "Enter sends, Ctrl-J
        // adds a line" impossible to implement. With nonl() Return stays
        // \r and the two are distinct keys again.
        nonl();

        keypad(stdscr, TRUE);
        curs_set(1);

        // **A key read that gives up, which is what makes the worker
        // visible.** The relay reports through a descriptor and the loop has
        // to get back to it; a blocking getch() would hold the reply until
        // somebody happened to press something. 50ms is under the ~100ms
        // where a keystroke starts to feel late, and twenty wake-ups a
        // second that redraw only when an event arrived cost nothing.
        //
        // The honest alternative is to wait on the pty and the relay's pipe
        // together, which is what a reactor is for -- and it means giving up
        // getch() for a key decoder of our own, because curses owns the
        // terminal's input. Not worth it at this size; worth revisiting if
        // the poll ever shows up in a profile.
        timeout(50);

        if(has_colors()) {
            start_color();
            use_default_colors();

            // -1 is the terminal's own colour, so the transcript reads as
            // whatever the user chose rather than as this program's opinion.
            init_pair(plain, -1, -1);
            init_pair(you_tone, COLOR_CYAN, -1);
            init_pair(note_tone, COLOR_YELLOW, -1);
            init_pair(tool_tone, COLOR_MAGENTA, -1);
            init_pair(build_tone, COLOR_GREEN, -1);
            init_pair(head_tone, COLOR_BLACK, COLOR_WHITE);

            m_colour = true;
        }

        layout();
    }

    ~ui() { endwin(); }

    ui(const ui&) = delete;
    ui& operator=(const ui&) = delete;

    /** Rebuild for the terminal's current size. */
    void layout() {
        getmaxyx(stdscr, m_rows, m_cols);

        if(m_cols < 1) m_cols = 1;

        erase();
        refresh();
    }

    /**
     * Draw everything.
     *
     * One function rather than a set of partial updates, because the
     * transcript is re-wrapped from text on every draw -- which is the whole
     * point of keeping it as text, and cheap enough at these sizes that
     * paying it on each keystroke is simpler than tracking what changed.
     */
    void draw(const transcript& t, const state& s, const std::string& prompt,
              std::size_t caret)
    {
        const int prompt_lines = lines_in(prompt);
        const int prompt_rows = prompt_lines > 5 ? 5 : prompt_lines;

        // header, rule, [transcript], rule, prompt, status
        int view = m_rows - 4 - prompt_rows;

        if(view < 1) view = 1;

        erase();

        header(s);

        mvhline(1, 0, ACS_HLINE, m_cols);

        // **Into stdscr, not into a pad.** A pad has to be blitted with
        // prefresh, which writes straight to the physical screen -- and then
        // the refresh() below pushes stdscr over the top, whose copy of those
        // rows is blank because of the erase() above. The transcript was
        // painted and wiped on every draw, which is why nothing but the
        // header, the prompt and the status line was ever visible.
        //
        // Keeping the transcript as text is what makes this the simple fix
        // rather than a refresh-ordering puzzle: the lines are re-wrapped
        // here anyway, so the visible slice can go straight into the window
        // everything else is in. One window, one refresh, nothing overlapping.
        const std::vector<std::string> ls = lines(t);

        const std::size_t first =
            ls.size() > std::size_t(view) ? ls.size() - std::size_t(view) : 0;

        for(std::size_t i = first; i < ls.size(); i++)
            mvaddstr(2 + int(i - first), 0, ls[i].c_str());

        mvhline(2 + view, 0, ACS_HLINE, m_cols);

        const int prow = 3 + view;

        const int caret_y = show_prompt(prompt, caret, prow, prompt_rows);
        const int caret_x = caret_col(prompt, caret);

        status(s);

        // **Last, after everything is painted.** `mvaddstr` leaves the cursor
        // where it finished writing, so drawing the status line after the
        // prompt parks the hardware cursor at the end of the status line --
        // which is where it sat, one row below the prompt it belongs in.
        // Anything that writes after this moves it again.
        move(caret_y, caret_x + 2);

        refresh();
    }

private:
    static int lines_in(const std::string& s) {
        int n = 1;

        for(char c : s) if(c == '\n') n++;

        return n;
    }

    void header(const state& s) {
        if(m_colour) attron(COLOR_PAIR(head_tone));

        // The whole width, so the bar reads as a bar rather than as text that
        // happens to be inverted.
        std::string bar = " " + s.task;

        const std::string right = s.model + "  " + s.mode_name() +
            (s.build ? "  build" : "") + " ";

        while(int(bar.size()) + int(right.size()) < m_cols) bar += " ";

        bar += right;

        if(int(bar.size()) > m_cols) bar = bar.substr(0, std::size_t(m_cols));

        mvaddstr(0, 0, bar.c_str());

        if(m_colour) attroff(COLOR_PAIR(head_tone));
    }

    /**
     * The transcript as screen lines, wrapped to the current width.
     *
     * Colour is lost by going through plain strings, which is the price of
     * the simplification above and is worth naming: the tags carry the
     * distinction instead, and restoring colour means returning a line and
     * its tone together rather than going back to a pad.
     */
    std::vector<std::string> lines(const transcript& t) const {
        std::vector<std::string> out;

        for(const entry& e : t.entries()) {
            const char* tag = "";

            switch(e.k) {
            case kind::you:   tag = "> ";  break;
            case kind::model: tag = "";    break;
            case kind::tool:  tag = ":: "; break;
            case kind::build: tag = "|  "; break;
            case kind::edit:  tag = "** "; break;
            case kind::note:  tag = "-- "; break;
            }

            for(const std::string& l : wrap(std::string(tag) + e.text))
                out.push_back(l);
        }

        return out;
    }

    /**
     * Break text to the current width, on character boundaries.
     *
     * `utf8::length` rather than `std::string::size`, because a wrap that
     * counts bytes cuts a multi-byte character in half and curses then draws
     * the halves as two cells of nothing.
     */
    std::vector<std::string> wrap(const std::string& text) const {
        std::vector<std::string> out;

        const std::size_t width = m_cols > 1 ? std::size_t(m_cols - 1) : 1;

        std::size_t from = 0;

        while(from < text.size()) {
            const std::size_t nl = text.find('\n', from);
            const std::size_t end = nl == std::string::npos ? text.size() : nl;

            std::string line = text.substr(from, end - from);

            if(line.empty()) out.push_back("");

            while(!line.empty()) {
                const std::string piece = head(line, width);

                out.push_back(piece);

                line = line.substr(piece.size());
            }

            from = nl == std::string::npos ? text.size() : nl + 1;
        }

        if(out.empty()) out.push_back("");

        return out;
    }

    /** Which line of the prompt the caret is on, counted from its start. */
    static int caret_line(const std::string& prompt, std::size_t caret) {
        int cl = 0;

        for(std::size_t i = 0; i < caret && i < prompt.size(); i++)
            if(prompt[i] == '\n') cl++;

        return cl;
    }

    /** The caret's column, in characters rather than bytes. */
    static std::size_t caret_col(const std::string& prompt, std::size_t caret) {
        std::size_t line_start = 0;

        for(std::size_t i = 0; i < caret && i < prompt.size(); i++)
            if(prompt[i] == '\n') line_start = i + 1;

        return chars_in(prompt.substr(line_start), caret - line_start);
    }

    /** Draws the prompt and returns the screen row the caret belongs on. */
    int show_prompt(const std::string& prompt, std::size_t caret,
                    int row, int rows)
    {
        const int cl = caret_line(prompt, caret);

        std::vector<std::string> ls;

        std::size_t from = 0;

        for(;;) {
            const std::size_t nl = prompt.find('\n', from);

            ls.push_back(prompt.substr(from, nl == std::string::npos
                                             ? std::string::npos : nl - from));

            if(nl == std::string::npos) break;

            from = nl + 1;
        }

        const int first = cl >= rows ? cl - rows + 1 : 0;

        for(int i = 0; i < rows; i++) {
            const std::size_t at = std::size_t(first + i);

            const std::string text = at < ls.size() ? ls[at] : "";

            mvaddstr(row + i, 0, (i == 0 ? "> " : "  "));
            addstr(head(text, std::size_t(m_cols - 3)).c_str());
        }

        return row + (cl - first);
    }

    void status(const state& s) {
        char buf[256];

        std::snprintf(buf, sizeof(buf),
                      " %llu in / %llu out   ctx %u/%u   rounds %u   attempts %u",
                      s.tokens_in, s.tokens_out, s.context_used,
                      s.context_total, s.rounds, s.attempts);

        std::string line = buf;

        // **Whether a request is in flight belongs on screen.** A harness
        // that looks identical while waiting and while idle is the thing the
        // worker was added to avoid -- the user cannot tell "thinking" from
        // "did not take my Return".
        line += s.working ? "   working" : "   idle";

        if(int(line.size()) > m_cols) line = line.substr(0, std::size_t(m_cols));

        if(m_colour) attron(COLOR_PAIR(note_tone));

        mvaddstr(m_rows - 1, 0, line.c_str());

        if(m_colour) attroff(COLOR_PAIR(note_tone));
    }

    int m_rows = 0;
    int m_cols = 0;
    bool m_colour = false;
};

/** Whether a submitted line is a command rather than a prompt. */
bool is_command(const std::string& line) {
    // `//` is how a prompt says its first character is a slash. Borrowed from
    // IRC because it is the convention people already know, and a coding
    // harness will meet a leading slash the first time someone pastes an
    // absolute path.
    return line.size() > 0 && line[0] == '/' &&
           !(line.size() > 1 && line[1] == '/');
}

/** A command's text with the leading slash removed, split on spaces. */
std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;

    std::size_t i = 0;

    while(i < line.size()) {
        while(i < line.size() && line[i] == ' ') i++;

        const std::size_t start = i;

        while(i < line.size() && line[i] != ' ') i++;

        if(i > start) out.push_back(line.substr(start, i - start));
    }

    return out;
}

const char* HELP[] = {
    "/help            this",
    "/quit            leave",
    "/mode            edit format, build, rounds, attempts",
    "/mode whole      send whole files",
    "/mode hunks      send search/replace hunks",
    "/mode build on   run the build between attempts",
    "/model           the endpoint and model, or /model NAME to choose",
    "/stats           tokens, context, rounds, attempts",
    "/files           the files offered on the command line",
    "",
    "A prompt that starts with a slash is written //like-this.",
    "Enter sends; Ctrl-J adds a line without sending; Ctrl-U clears it.",
    "Anything else is sent to the model.  Nothing is written without --yes.",
    0
};

/**
 * Returns false when the command was /quit.
 *
 * `options` is read as well as `state` so an answer can name what the program
 * was started with -- "build on" with no `--build` is a thing a user needs
 * told at the moment they type it, not one request later when nothing ran.
 */
bool command(const std::string& line, state& s, transcript& t,
             const options& o)
{
    const std::vector<std::string> w = words(line.substr(1));

    if(w.empty()) { t.add(kind::note, "a slash on its own does nothing"); return true; }

    const std::string& c = w[0];

    if(c == "quit" || c == "q") return false;

    if(c == "help" || c == "h") {
        for(const char** p = HELP; *p; p++) t.add(kind::note, *p);

        return true;
    }

    if(c == "mode") {
        if(w.size() == 1) {
            t.add(kind::note, "edits " + s.mode_name() +
                              ", build " + (s.build ? "on" : "off") +
                              ", rounds " + std::to_string(s.rounds) +
                              ", attempts " + std::to_string(s.attempts));

            return true;
        }

        if(w[1] == "whole") { s.edits = jcode::format::whole; t.add(kind::note, "edits whole"); return true; }
        if(w[1] == "hunks") { s.edits = jcode::format::hunks; t.add(kind::note, "edits hunks"); return true; }

        if(w[1] == "build" && w.size() > 2) {
            s.build = w[2] == "on";

            t.add(kind::note, std::string("build ") + (s.build ? "on" : "off"));

            // The build tool exists only when a command was named, so this
            // switch has nothing to switch.  Said here rather than silently
            // doing nothing a request later.
            if(s.build && o.build.empty())
                t.add(kind::note, "but no --build was given, so there is "
                                  "nothing to run");

            return true;
        }

        if(w[1] == "rounds" && w.size() > 2) {
            s.rounds = unsigned(std::atoi(w[2].c_str()));
            t.add(kind::note, "rounds " + std::to_string(s.rounds));
            return true;
        }

        if(w[1] == "attempts" && w.size() > 2) {
            s.attempts = unsigned(std::atoi(w[2].c_str()));
            t.add(kind::note, "attempts " + std::to_string(s.attempts));
            return true;
        }

        t.add(kind::note, "no such mode: " + w[1]);

        return true;
    }

    if(c == "model") {
        if(w.size() > 1) {
            s.model = w[1];

            t.add(kind::note, "model " + s.model +
                              " -- not checked against the endpoint yet");
        }
        else {
            // There is an endpoint now, but asking it costs a `/v1/models`
            // request this phase does not make. Naming the URL is the honest
            // half of the answer; the list is #450.
            t.add(kind::note, "model " + s.model + " at " + o.url);
            t.add(kind::note, "the endpoint's own list is #450");
        }

        return true;
    }

    if(c == "stats") {
        t.add(kind::note, std::to_string(s.tokens_in) + " tokens in, " +
                          std::to_string(s.tokens_out) + " out, context " +
                          std::to_string(s.context_used) + " of " +
                          std::to_string(s.context_total));

        // Zero because nothing was counted is a different fact from zero
        // because nothing was sent, and only one of them is a defect.
        if(!s.context_total)
            t.add(kind::note, "all zero: nothing has been sent yet");

        return true;
    }

    if(c == "files") {
        // The layout is decided on the worker thread and reported as prose,
        // so there is no structured copy to show here yet. #450 carries the
        // plan back and this grows a table.
        if(o.names.empty())
            t.add(kind::note, "no files were named on the command line");
        else {
            t.add(kind::note, std::to_string(o.names.size()) +
                              " file(s) offered, under " + o.root + ":");

            for(const std::string& n : o.names) t.add(kind::note, "  " + n);

            t.add(kind::note, "what actually fitted is said when a request "
                              "goes out; a table is #450");
        }

        return true;
    }

    t.add(kind::note, "no such command: /" + c + " -- try /help");

    return true;
}

/**
 * One event, into the state the UI draws from.
 *
 * The only place a worker's output reaches `state` or `transcript`, and it
 * runs on the UI thread -- which is what makes neither of them need a lock.
 */
void absorb(const event& e, state& s, transcript& t) {
    if(e.n == news::counts) {
        s.tokens_in = e.tokens_in;
        s.tokens_out = e.tokens_out;
        s.context_used = e.context_used;
        s.context_total = e.context_total;

        return;
    }

    t.add(e.k, e.text);
}

volatile std::sig_atomic_t g_interrupted = 0;

void on_signal(int) { g_interrupted = 1; }

void usage(std::ostream& o, const char* me) {
    o << "usage: " << me << " [options] [file ...]\n"
      << "\n"
      << "  jcode with a transcript that stays put.  Type a request; the\n"
      << "  reply, the tool calls, the build and the edits all land in the\n"
      << "  transcript while the prompt goes on taking keys.\n"
      << "\n"
      << "  Nothing is written without --yes: the confirm dialog is #449,\n"
      << "  and until it exists an edit is announced and not applied.\n"
      << "\n"
      << "  --url URL       an OpenAI-compatible endpoint\n"
      << "                  (default http://127.0.0.1:8080/v1)\n"
      << "  --model NAME    which model to ask for\n"
      << "  --root DIR      what the filenames are relative to, and the\n"
      << "                  directory no edit may escape\n"
      << "  --context N     the context to lay out against, in tokens\n"
      << "  --tokens N      max_tokens; 0 leaves it to the server\n"
      << "  --timeout SECS  how long to wait for a reply (default 600)\n"
      << "  --build CMD     a build command the model may run, split on\n"
      << "                  whitespace; no shell is involved\n"
      << "  --yes           write without asking\n"
      << "  --task TEXT     what the top line says\n"
      << "  --help\n"
      << "\n"
      << "  Enter sends, Ctrl-J adds a line, Ctrl-U clears the prompt,\n"
      << "  Ctrl-C or /quit leaves.\n";
}

} // namespace

int main(int argc, char** argv) {
    state s;
    options o;

    for(int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const bool has_next = i + 1 < argc;

        if(a == "--help" || a == "-h") { usage(std::cout, argv[0]); return 0; }

        else if(a == "--task" && has_next)    s.task = argv[++i];
        else if(a == "--url" && has_next)     o.url = argv[++i];
        else if(a == "--model" && has_next)   o.model = argv[++i];
        else if(a == "--root" && has_next)    o.root = argv[++i];
        else if(a == "--context" && has_next) o.context = std::size_t(std::atoll(argv[++i]));
        else if(a == "--tokens" && has_next)  o.tokens = unsigned(std::atoi(argv[++i]));
        else if(a == "--timeout" && has_next) o.timeout = std::atof(argv[++i]);
        else if(a == "--build" && has_next)   o.build = argv[++i];
        else if(a == "--yes")                 o.yes = true;

        else if(!a.empty() && a[0] == '-') {
            // **Before initscr, and that is the point.** A usage error from
            // inside curses would be painted over the screen it just set up
            // and lost when endwin put the terminal back.
            std::cerr << argv[0] << ": no such option: " << a << "\n";

            usage(std::cerr, argv[0]);

            return 1;
        }

        else o.names.push_back(a);
    }

    s.model = o.model.empty() ? std::string("(server's default)") : o.model;

    // SIGINT only sets a flag: endwin() has to run from the normal path or
    // the terminal is left without echo, and a handler cannot safely do it.
    struct sigaction sa;

    std::memset(&sa, 0, sizeof(sa));

    sa.sa_handler = on_signal;

    sigaction(SIGINT, &sa, 0);

    transcript t;

    t.add(kind::note, "juanaco.  /help lists the commands.");
    t.add(kind::note, "asking " + o.url +
                      (o.names.empty()
                           ? std::string(", with no files offered")
                           : ", with " + std::to_string(o.names.size()) +
                             " file(s) under " + o.root));

    if(!o.yes)
        t.add(kind::note, "no --yes, so an edit is announced and not written "
                          "(#449)");

    ui sc;

    // **One at a time, and held by the UI thread.** The relay is created when
    // a request goes out and destroyed when it has been drained; `worker`
    // being set is the same fact as `state::working`, which exists only
    // because `status` draws from `state` and not from here.
    std::unique_ptr<sys::relay<event> > worker;

    std::string prompt;
    std::size_t caret = 0;

    sc.draw(t, s, prompt, caret);

    for(;;) {
        if(g_interrupted) break;

        // **Drained before a key is read, not after.** What the worker said
        // has to reach the screen whether or not anybody is typing, and
        // getch() is what gives this loop its pulse -- see timeout() in the
        // constructor.
        if(worker) {
            bool moved = false;

            for(const event& e : worker->take()) { absorb(e, s, t); moved = true; }

            if(worker->done()) {
                // **take() and done() are two facts about a thread that is
                // still running, and the order they are read in decides what
                // gets lost.** finish() runs after every emit, so one further
                // take after seeing done sees everything; asking done() first
                // and taking once drops whatever was emitted while the loop
                // was busy with the previous batch. That is sys::pump's
                // reasoning, and this loop is the same shape without the
                // reactor.
                for(const event& e : worker->take()) absorb(e, s, t);

                const std::string why = worker->why();

                if(!why.empty()) t.add(kind::note, "it failed: " + why);

                worker->join();
                worker.reset();

                s.working = false;
                moved = true;
            }

            if(moved) sc.draw(t, s, prompt, caret);
        }

        const int c = getch();

        if(c == KEY_RESIZE) { sc.layout(); sc.draw(t, s, prompt, caret); continue; }

        if(c == ERR) continue;

        // Ctrl-D on an empty prompt leaves, which is what a shell does.
        if(c == 4 && prompt.empty()) break;

        // Return only. '\n' is Ctrl-J and is handled below; see nonl() above
        // for why they are now different keys.
        if(c == '\r' || c == KEY_ENTER) {
            if(prompt.empty()) { sc.draw(t, s, prompt, caret); continue; }

            // **A request in flight blocks the next one, and the line stays
            // where it was typed.** Clearing it to say "not sent" would make
            // somebody retype a paragraph, and queueing it would open a
            // second conversation against a cache that has not finished the
            // first. Slash commands still work: they are local, and the job
            // was copied when it went out, so a `/mode` now takes effect on
            // the next request rather than halfway through this one.
            if(worker && !is_command(prompt)) {
                t.add(kind::note, "still working -- your line is kept; "
                                  "cancelling is #448");

                sc.draw(t, s, prompt, caret);

                continue;
            }

            const std::string line = prompt;

            prompt.clear();
            caret = 0;

            if(is_command(line)) {
                if(!command(line, s, t, o)) break;
            }
            else {
                // `//x` was a prompt whose first character is a slash.
                const std::string text =
                    line.size() > 1 && line[0] == '/' && line[1] == '/'
                        ? line.substr(1) : line;

                t.add(kind::you, text);

                if(s.task == "(no task yet)") s.task = text;

                worker.reset(new sys::relay<event>());

                // By value into the thread: see `job`, which is copied for
                // exactly this.
                const job j = compose(o, s, text);

                worker->start([j](sys::relay<event>& r) { work(r, j); });

                s.working = true;
            }

            sc.draw(t, s, prompt, caret);

            continue;
        }

        // Ctrl-J: a newline in the prompt rather than a submission. Enter is
        // taken, and a coding request is a paragraph.
        if(c == 10) {
            prompt.insert(caret, 1, '\n');
            caret++;
            sc.draw(t, s, prompt, caret);
            continue;
        }

        // **Ctrl-U clears the prompt, and here that is load-bearing rather
        // than a convenience.** A line refused because a request is in
        // flight stays in the prompt, which is the kind thing to do with a
        // paragraph -- and with no way to clear it the next thing typed is
        // appended to it. `/quit` after a refusal became
        // "and so is this/quit", which is not a command, so it was refused
        // in turn and Ctrl-C was the only way out of the program. The pty
        // test found that, and it is why the refusal and the clear had to
        // arrive together.
        if(c == 21) {
            prompt.clear();
            caret = 0;

            sc.draw(t, s, prompt, caret);

            continue;
        }

        if(c == KEY_BACKSPACE || c == 127 || c == 8) {
            if(caret) {
                // Back over a whole character, not a byte: a continuation
                // byte is 10xxxxxx, so step until the lead byte.
                std::size_t at = caret - 1;

                while(at > 0 && (prompt[at] & 0xc0) == 0x80) at--;

                prompt.erase(at, caret - at);
                caret = at;
            }

            sc.draw(t, s, prompt, caret);

            continue;
        }

        if(c == KEY_LEFT)  { if(caret) { std::size_t at = caret - 1; while(at > 0 && (prompt[at] & 0xc0) == 0x80) at--; caret = at; } sc.draw(t, s, prompt, caret); continue; }
        if(c == KEY_RIGHT) { if(caret < prompt.size()) { std::size_t at = caret + 1; while(at < prompt.size() && (prompt[at] & 0xc0) == 0x80) at++; caret = at; } sc.draw(t, s, prompt, caret); continue; }

        // Escape is reserved: #448 gives it cancellation, and nothing else
        // should take it before then.
        if(c == 27) continue;

        if(c >= 32 && c < 256) {
            prompt.insert(caret, 1, char(c));
            caret++;
            sc.draw(t, s, prompt, caret);
        }
    }

    // **Leaving with a request still in flight.**
    //
    // relay's destructor joins, and a worker inside a ten-minute HTTP read
    // does not come back when it is asked to: wanted() is read between items
    // and a blocking read is not between items. So `/quit` at the wrong
    // moment would restore the terminal and then hold the shell for ten
    // minutes with nothing on screen to say why.
    //
    // Ask, wait two seconds, and then stop waiting. A worker that was
    // writing a file or running a build finishes well inside that and gets
    // joined properly; one in a read never will, and the process has nothing
    // left to do -- the socket is the kernel's to close.
    //
    // **What this does not promise.** jcode::apply truncates and writes
    // directly rather than renaming a temporary over the target, so a worker
    // killed in the middle of one leaves a short file. The window is a single
    // ofstream wide, and two seconds is a great deal longer than it -- but it
    // is a window, and closing it means atomic replacement inside apply(),
    // which is the library's to do and not this exit path's.
    if(worker) {
        worker->stop();

        for(int i = 0; i < 20 && !worker->done(); i++) ::usleep(100 * 1000);

        if(!worker->done()) {
            // endwin first, so the message lands on a terminal that can
            // show it.
            endwin();

            std::cerr << "juanaco: left with a request still in flight; "
                      << "not waiting for it\n";

            std::_Exit(0);
        }
    }

    return 0;
}
