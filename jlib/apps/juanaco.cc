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
 * **The skeleton only** (#446, phase 2 of #444). Nothing here talks to a
 * model: the layout, the prompt, the transcript and the slash commands are
 * all that is present, and every one of them can be driven by `make check`
 * through the pty harness from #445 with no GGUF, no server and no network.
 * The worker thread arrives in #447 and streaming in #448.
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

#include <jlib/util/utf8.hh>

#include <ncurses.h>

#include <clocale>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace jcode = jlib::apps::jcode;

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

    jcode::format edits = jcode::format::hunks;

    bool build = false;
    unsigned int rounds = 8;
    unsigned int attempts = 3;

    // Counters for /stats. Zero until there is something to count, which is
    // #447 onwards -- they are here so the command has a shape now rather
    // than a stub that gets rewritten.
    unsigned long long tokens_in = 0;
    unsigned long long tokens_out = 0;
    unsigned int context_used = 0;
    unsigned int context_total = 0;

    std::string mode_name() const {
        return edits == jcode::format::whole ? "whole" : "hunks";
    }
};

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
    "/model           list models, or /model NAME to choose",
    "/stats           tokens, context, rounds, attempts",
    "/files           what was put in context, and what was trimmed",
    "",
    "A prompt that starts with a slash is written //like-this.",
    "Enter sends; Ctrl-J adds a line without sending.",
    0
};

/** Returns false when the command was /quit. */
bool command(const std::string& line, state& s, transcript& t) {
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
        // The list comes from the endpoint, which this phase does not have.
        // Saying so is better than an empty list, which would read as "no
        // models" rather than as "not asked yet" (#450).
        if(w.size() > 1) {
            s.model = w[1];

            t.add(kind::note, "model " + s.model +
                              " -- not checked against the endpoint yet");
        }
        else t.add(kind::note, "no endpoint yet, so no list to show (#447)");

        return true;
    }

    if(c == "stats") {
        t.add(kind::note, std::to_string(s.tokens_in) + " tokens in, " +
                          std::to_string(s.tokens_out) + " out, context " +
                          std::to_string(s.context_used) + " of " +
                          std::to_string(s.context_total));

        if(!s.context_total)
            t.add(kind::note, "all zero: nothing has been sent yet (#447)");

        return true;
    }

    if(c == "files") {
        t.add(kind::note, "no plan yet, so nothing in context (#447)");

        return true;
    }

    t.add(kind::note, "no such command: /" + c + " -- try /help");

    return true;
}

volatile std::sig_atomic_t g_interrupted = 0;

void on_signal(int) { g_interrupted = 1; }

} // namespace

int main(int argc, char** argv) {
    for(int i = 1; i < argc; i++) {
        const std::string a = argv[i];

        if(a == "--help" || a == "-h") {
            std::cout
                << "usage: " << argv[0] << " [--task TEXT]\n"
                << "\n"
                << "  jcode with a transcript that stays put.  The skeleton:\n"
                << "  layout, prompt and slash commands, and no model yet.\n"
                << "\n"
                << "  --task TEXT   what the top line says\n"
                << "  --help\n";

            return 0;
        }
    }

    state s;

    for(int i = 1; i < argc - 1; i++)
        if(std::string(argv[i]) == "--task") s.task = argv[i + 1];

    // SIGINT only sets a flag: endwin() has to run from the normal path or
    // the terminal is left without echo, and a handler cannot safely do it.
    struct sigaction sa;

    std::memset(&sa, 0, sizeof(sa));

    sa.sa_handler = on_signal;

    sigaction(SIGINT, &sa, 0);

    transcript t;

    t.add(kind::note, "juanaco -- the skeleton.  /help lists the commands.");
    t.add(kind::note, "Nothing is wired to a model yet (#446).");

    ui sc;

    std::string prompt;
    std::size_t caret = 0;

    sc.draw(t, s, prompt, caret);

    for(;;) {
        if(g_interrupted) break;

        const int c = getch();

        if(c == KEY_RESIZE) { sc.layout(); sc.draw(t, s, prompt, caret); continue; }

        if(c == ERR) continue;

        // Ctrl-D on an empty prompt leaves, which is what a shell does.
        if(c == 4 && prompt.empty()) break;

        // Return only. '\n' is Ctrl-J and is handled below; see nonl() above
        // for why they are now different keys.
        if(c == '\r' || c == KEY_ENTER) {
            if(prompt.empty()) { sc.draw(t, s, prompt, caret); continue; }

            const std::string line = prompt;

            prompt.clear();
            caret = 0;

            if(is_command(line)) {
                if(!command(line, s, t)) break;
            }
            else {
                // `//x` was a prompt whose first character is a slash.
                const std::string text =
                    line.size() > 1 && line[0] == '/' && line[1] == '/'
                        ? line.substr(1) : line;

                t.add(kind::you, text);

                if(s.task == "(no task yet)") s.task = text;

                t.add(kind::note, "nothing to send it to yet (#447)");
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

    return 0;
}
