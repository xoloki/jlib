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
 * jalpaca -- jchat with a transcript that stays put.
 *
 * A scrolling transcript above, a prompt pinned below, and the reply arriving
 * into the transcript a token at a time. Escape stops a reply without ending
 * the conversation.
 *
 * ### Why this is a second program rather than a flag on the first
 *
 * jchat is the tested one. Its argument handling, its answers and its
 * conversation memory are checked in `app_jchat_test`, which drives it down a
 * pipe -- and a curses program cannot be driven down a pipe, because it wants a
 * terminal and refuses to start without one. Folding the two together would
 * mean either giving up that test or carrying two output paths through every
 * function in it.
 *
 * So the loop below is deliberately the same loop, and the pieces underneath --
 * gguf, tokenizer, chat, model, generate -- are the same pieces. What differs
 * is where the characters go.
 *
 * ### What curses buys beyond appearance
 *
 * Escape. Reading a keystroke *while* generating needs the terminal out of
 * canonical mode, which otherwise buffers input until Return; and having taken
 * it out, something has to put it back on exit, on an exception and on a
 * signal, or the shell is left with no echo. curses owns both halves. Doing it
 * by hand with tcsetattr is about forty lines and every one of them is a way to
 * strand a terminal.
 */

#include <jlib/ai/chat.hh>
#include <jlib/ai/generate.hh>
#include <jlib/ai/model.hh>
#include <jlib/ai/tokenizer.hh>

#ifdef HAVE_METAL
#include <jlib/metal/backend.hh>
#endif

#include <ncurses.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace ai = jlib::ai;

namespace {

volatile std::sig_atomic_t g_interrupted = 0;

void interrupted(int) { g_interrupted = 1; }

double now() {
    using namespace std::chrono;

    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

/**
  * Which colour a line is wearing.
  *
  * There is deliberately no tone for the reply: it is most of what is on the
  * screen, so colouring it colours the screen.  Colour marks what is *not* the
  * reply -- the question that prompted it and the note that closes it -- which
  * is what makes those two findable when scrolling back through a long
  * transcript.
  */
enum tone { plain = 1, you = 2, note = 3 };

/**
 * Logical keys that arrive as escape sequences rather than as keys.
 *
 * Above ncurses' own range, which stops well below this, so they cannot
 * collide with a KEY_ constant.
 */
enum { KEY_WORD_LEFT = 0x1000, KEY_WORD_RIGHT, KEY_BARE_ESC };

/**
 * Fewest tokens worth generating a reply into.
 *
 * Below this there is no answer to be had, only the beginning of one, and
 * saying the prompt does not fit is more use than a truncated fragment of a
 * sentence.
 */
const unsigned int MIN_REPLY = 16;

/**
 * The screen: a transcript with scrollback, and a prompt that stays put.
 *
 * endwin() runs from the destructor, so a throw on the way out of main still
 * hands the terminal back. That is the whole reason this is a class.
 *
 * ### Why the transcript is a pad
 *
 * A window scrolls and forgets: what leaves the top is gone, so there is
 * nothing for PageUp to go back to. A pad holds the whole transcript and the
 * window shows a moving view of it. The depth is bounded -- PAD_ROWS lines,
 * after which the oldest scroll off -- because a pad is allocated up front and
 * a conversation is not.
 */
class screen {
public:
    static const int PAD_ROWS = 4000;

    screen() {
        initscr();
        cbreak();               // keys arrive unbuffered, which Escape needs
        noecho();               // the input window draws the line itself
        keypad(stdscr, TRUE);
        curs_set(1);

        if(has_colors()) {
            start_color();
            use_default_colors();

            // -1 is the terminal's own foreground and background, which
            // use_default_colors makes available: the reply then reads as
            // whatever the user set their terminal to, rather than as this
            // program's opinion of white.
            init_pair(plain, -1, -1);
            init_pair(you,   COLOR_CYAN, -1);
            init_pair(note,  COLOR_YELLOW, -1);

            m_colour = true;
        }

        layout();
    }

    ~screen() { endwin(); }

    screen(const screen&) = delete;
    screen& operator=(const screen&) = delete;

    /** Rebuild for the terminal's current size, keeping the transcript. */
    void layout() {
        getmaxyx(stdscr, m_rows, m_cols);

        if(m_input) delwin(m_input);

        m_view = m_rows - 2;

        if(m_view < 1) m_view = 1;

        // The pad is remade only when the width changes, since its contents are
        // wrapped to that width and cannot be rewrapped without re-rendering
        // everything -- which would need the transcript kept as text as well.
        if(!m_pad || m_cols != m_pad_cols) {
            if(m_pad) delwin(m_pad);

            m_pad = newpad(PAD_ROWS, m_cols);
            m_pad_cols = m_cols;
            m_used = 0;

            scrollok(m_pad, TRUE);
        }

        m_input = newwin(1, m_cols, m_rows - 1, 0);

        nodelay(m_input, TRUE);
        keypad(m_input, TRUE);

        mvhline(m_rows - 2, 0, ACS_HLINE, m_cols);
        refresh();

        show();
    }

    /** Add text to the transcript in a colour. */
    void say(const std::string& text, tone t = plain) {
        if(m_colour) wattron(m_pad, COLOR_PAIR(t));

        waddstr(m_pad, text.c_str());

        if(m_colour) wattroff(m_pad, COLOR_PAIR(t));

        int y = 0, x = 0;

        getyx(m_pad, y, x);

        m_used = y + (x > 0 ? 1 : 0);

        // Anything arriving while the reader is scrolled back should not yank
        // them to the bottom; anything arriving while they are at the bottom
        // should be visible.
        if(m_scroll == 0) show();
    }

    void say_line(const std::string& text, tone t = plain) { say(text + "\n", t); }

    /** Move the view; positive is towards the top. */
    void scroll_by(int lines) {
        m_scroll += lines;

        const int most = m_used - m_view;

        if(m_scroll > (most > 0 ? most : 0)) m_scroll = most > 0 ? most : 0;
        if(m_scroll < 0) m_scroll = 0;

        show();
    }

    void prompt(const std::string& line, std::size_t cursor) {
        werase(m_input);

        if(m_colour) wattron(m_input, COLOR_PAIR(you));

        mvwaddstr(m_input, 0, 0, "> ");

        if(m_colour) wattroff(m_input, COLOR_PAIR(you));

        // Only what fits, scrolled so the cursor is always on screen.
        const int room = m_cols - 3;
        std::size_t from = 0;

        if(room > 0 && cursor > std::size_t(room)) from = cursor - room;

        waddnstr(m_input, line.c_str() + from, room);
        wmove(m_input, 0, int(2 + (cursor - from)));
        wrefresh(m_input);
    }

    /**
     * The next key, with escape sequences folded into single values.
     *
     * A bare Escape and the start of a sequence are the same byte, so the only
     * way to tell them apart is to wait a moment and see whether anything
     * follows. Option-arrow sends `ESC b` and `ESC f` on some terminals and
     * `ESC [1;3D` and `ESC [1;3C` on others; both are read here.
     *
     * @return ERR when nothing is waiting
     */
    int key() {
        const int c = wgetch(m_input);

        if(c == ERR) return ERR;

        // Extended keys. ncurses allocates a code for these from terminfo at
        // run time, so there is no constant to compare against and the name is
        // the only stable handle: Option-left is "kLFT3" and control-left
        // "kLFT5" on the terminals that distinguish them.
        //
        // Which of the two paths a sequence takes is not predictable. Measured
        // on one terminal in one run: Option-left and PageUp arrived assembled
        // as 545 and 339, while plain Up arrived as ESC, '[', 'A' -- because
        // with nodelay set ncurses cannot wait for the rest of a sequence and
        // only assembles the ones whose bytes had already landed. Both paths
        // have to work.
        if(c > KEY_MAX) {
            const char* n = keyname(c);

            if(n) {
                const std::string name = n;

                if(name == "kLFT3" || name == "kLFT5") return KEY_WORD_LEFT;
                if(name == "kRIT3" || name == "kRIT5") return KEY_WORD_RIGHT;
            }

            return ERR;
        }

        if(c != 27) return c;

        // Something within this window is part of a sequence; nothing is a
        // bare Escape. Long enough not to lose a fast terminal, short enough
        // not to be felt.
        wtimeout(m_input, 40);

        const int next = wgetch(m_input);

        int out = KEY_BARE_ESC;

        if(next == 'b') out = KEY_WORD_LEFT;
        else if(next == 'f') out = KEY_WORD_RIGHT;
        else if(next == '[') {
            // A CSI, and this has to read them all rather than only the ones
            // it wants. With nodelay set, ncurses does **not** assemble escape
            // sequences itself -- it cannot wait for the rest -- so every
            // arrow key arrives here as ESC, '[', and a letter. Recognising
            // only Option-arrow meant swallowing plain Up and Down, which is
            // exactly what the history test caught.
            std::string seq;

            for(int i = 0; i < 8; i++) {
                const int ch = wgetch(m_input);

                if(ch == ERR) break;

                seq += char(ch);

                if((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                   ch == '~')
                    break;
            }

            // ESC, '[' and then nothing: not a sequence this can name, and
            // the '[' is not a keystroke the user made either -- Escape then
            // a literal '[' is indistinguishable from a truncated CSI, and
            // guessing wrong inserts a bracket into somebody's question.
            if(seq.empty()) { nodelay(m_input, TRUE); return KEY_BARE_ESC; }

            const char last = seq[seq.size() - 1];

            // A modifier arrives as a parameter: ESC[1;3C is Option-right where
            // ESC[C is plain right. The semicolon is the whole difference.
            const bool modified = seq.find(';') != std::string::npos;

            switch(last) {
            case 'A': out = KEY_UP; break;
            case 'B': out = KEY_DOWN; break;
            case 'C': out = modified ? KEY_WORD_RIGHT : KEY_RIGHT; break;
            case 'D': out = modified ? KEY_WORD_LEFT : KEY_LEFT; break;
            case 'H': out = 1; break;                   // Home, as Ctrl-A
            case 'F': out = 5; break;                   // End, as Ctrl-E
            case '~':
                if(seq.compare(0, 1, "5") == 0) out = KEY_PPAGE;
                else if(seq.compare(0, 1, "6") == 0) out = KEY_NPAGE;
                else if(seq.compare(0, 1, "3") == 0) out = KEY_DC;
                else if(seq.compare(0, 1, "1") == 0) out = 1;
                else if(seq.compare(0, 1, "4") == 0) out = 5;
                else out = ERR;
                break;
            default: out = ERR; break;  // better ignored than guessed at
            }
        }
        else if(next != ERR) {
            // Escape followed by something this does not recognise. The
            // Escape was therefore a bare one, and the character after it is
            // an ordinary keystroke that has not been typed yet as far as the
            // editor is concerned -- so it goes back, rather than being eaten
            // as the tail of a sequence that turned out not to exist.
            //
            // This is also what stops the 40ms above from having to be right.
            // Measured: typing Escape and then a letter two thirds of a second
            // later still delivered the letter as `next`, so the window is not
            // honoured as exactly as the call reads. Without the pushback that
            // lost the letter *and* the Escape; with it, a window that is too
            // long costs nothing but a redraw.
            ungetch(next);
        }

        nodelay(m_input, TRUE);

        return out;
    }

    int columns() const { return m_cols; }
    int view_rows() const { return m_view; }

private:
    void show() {
        if(!m_pad) return;

        int top = m_used - m_view - m_scroll;

        if(top < 0) top = 0;

        prefresh(m_pad, top, 0, 0, 0, m_view - 1, m_cols - 1);
    }

    WINDOW* m_pad = 0;
    WINDOW* m_input = 0;

    int m_rows = 0;
    int m_cols = 0;
    int m_pad_cols = 0;
    int m_view = 0;

    /** Lines written to the pad, and how far back the view is. */
    int m_used = 0;
    int m_scroll = 0;

    bool m_colour = false;
};

/**
 * The prompt line: a buffer, a cursor, and a history.
 *
 * Not readline, and not trying to be. What is here is what a person reaches for
 * within the first minute: moving about, killing a word, and getting the last
 * thing back.
 */
class editor {
public:
    /** @return false at end of input */
    bool read(screen& s, std::string& out) {
        m_line.clear();
        m_cursor = 0;
        m_browse = m_history.size();

        for(;;) {
            s.prompt(m_line, m_cursor);

            const int c = s.key();

            if(c == ERR) {
                napms(15);

                if(g_interrupted) { g_interrupted = 0; return false; }

                continue;
            }

            switch(c) {
            case KEY_RESIZE: s.layout(); continue;

            case KEY_PPAGE: s.scroll_by(s.view_rows() / 2); continue;
            case KEY_NPAGE: s.scroll_by(-(s.view_rows() / 2)); continue;

            case KEY_UP:   recall(-1); continue;
            case KEY_DOWN: recall(1); continue;

            case KEY_LEFT:  if(m_cursor) m_cursor--; continue;
            case KEY_RIGHT: if(m_cursor < m_line.size()) m_cursor++; continue;

            case KEY_WORD_LEFT:  m_cursor = word_left(); continue;
            case KEY_WORD_RIGHT: m_cursor = word_right(); continue;

            case 1:  m_cursor = 0; continue;                  // Ctrl-A
            case 5:  m_cursor = m_line.size(); continue;      // Ctrl-E
            case 23: kill_word(); continue;                   // Ctrl-W

            case 11: m_line.erase(m_cursor); continue;        // Ctrl-K
            case 21: m_line.erase(0, m_cursor); m_cursor = 0; continue;   // Ctrl-U

            case 4: return false;                             // Ctrl-D

            case KEY_BARE_ESC: m_line.clear(); m_cursor = 0; continue;

            case KEY_BACKSPACE:
            case 127:
            case 8:
                if(m_cursor) { m_line.erase(--m_cursor, 1); }
                continue;

            case KEY_DC:
                if(m_cursor < m_line.size()) m_line.erase(m_cursor, 1);
                continue;

            case '\n':
            case '\r':
                out = m_line;

                // Kept only if it is not what was just said, which is what
                // stops a repeated question filling the history with itself.
                if(!m_line.empty() &&
                   (m_history.empty() || m_history.back() != m_line))
                    m_history.push_back(m_line);

                return true;

            default:
                if(c >= 32 && c < 256) {
                    m_line.insert(m_cursor, 1, char(c));
                    m_cursor++;
                }
                continue;
            }
        }
    }

private:
    static bool part_of_word(char c) { return c != ' ' && c != '\t'; }

    std::size_t word_left() const {
        std::size_t at = m_cursor;

        while(at && !part_of_word(m_line[at - 1])) at--;
        while(at && part_of_word(m_line[at - 1])) at--;

        return at;
    }

    std::size_t word_right() const {
        std::size_t at = m_cursor;

        while(at < m_line.size() && part_of_word(m_line[at])) at++;
        while(at < m_line.size() && !part_of_word(m_line[at])) at++;

        return at;
    }

    void kill_word() {
        const std::size_t to = word_left();

        m_line.erase(to, m_cursor - to);
        m_cursor = to;
    }

    /** Step through the history; -1 is older. */
    void recall(int by) {
        if(m_history.empty()) return;

        long at = long(m_browse) + by;

        if(at < 0) at = 0;
        if(at > long(m_history.size())) at = long(m_history.size());

        m_browse = std::size_t(at);

        // Past the newest is the empty line the person was typing on, which is
        // where Down should end up rather than sticking on the last entry.
        m_line = m_browse < m_history.size() ? m_history[m_browse] : std::string();
        m_cursor = m_line.size();
    }

    std::string m_line;
    std::size_t m_cursor = 0;

    std::vector<std::string> m_history;
    std::size_t m_browse = 0;
};

struct options {
    std::string model;
    std::string system;

    unsigned int tokens = 512;

    ai::sampler::config sampling;
};

bool parse(int argc, char** argv, options& o) {
    int i = 1;

    for(; i < argc; i++) {
        const std::string a = argv[i];

        if(a == "--help" || a == "-h") {
            std::cout << "usage: " << argv[0] << " [options] <model.gguf>\n"
                      << "\n"
                      << "  A transcript above, a prompt below.  Escape stops a\n"
                      << "  reply; Ctrl-D quits.\n"
                      << "\n"
                      << "  PageUp/PageDown  scroll the transcript\n"
                      << "  Up/Down          walk back through what was asked\n"
                      << "  Ctrl-A/Ctrl-E    start and end of the line\n"
                      << "  Ctrl-W           kill the word behind the cursor\n"
                      << "  Ctrl-U/Ctrl-K    kill to the start, to the end\n"
                      << "  Opt-Left/Right   move a word at a time\n"
                      << "\n"
                      << "  --system TEXT   a system turn before the conversation\n"
                      << "  --tokens N      room always kept free for a reply,\n"
                      << "                  in tokens (default 512)\n"
                      << "  --temp F        0 is greedy (default 0.8)\n"
                      << "  --top-k N       (default 40)\n"
                      << "  --top-p F       (default 0.95)\n"
                      << "  --seed N\n";

            std::exit(0);
        }
        else if(a.size() > 2 && a.compare(0, 2, "--") == 0) {
            if(i + 1 >= argc) {
                std::cerr << "jalpaca: " << a << " wants a value\n";

                return false;
            }

            const std::string v = argv[++i];

            if(a == "--system") o.system = v;
            else if(a == "--tokens") o.tokens = unsigned(std::atoi(v.c_str()));
            else if(a == "--temp") o.sampling.temperature = float(std::atof(v.c_str()));
            else if(a == "--top-k") o.sampling.top_k = unsigned(std::atoi(v.c_str()));
            else if(a == "--top-p") o.sampling.top_p = float(std::atof(v.c_str()));
            else if(a == "--seed") o.sampling.seed = std::uint64_t(std::atoll(v.c_str()));
            else {
                std::cerr << "jalpaca: no such option " << a << "\n";

                return false;
            }
        }
        else break;
    }

    if(i >= argc) {
        std::cerr << "usage: " << argv[0] << " [options] <model.gguf>\n";

        return false;
    }

    o.model = argv[i];

    return true;
}

template<typename T>
int converse(ai::backend<T>& b, const ai::gguf& g, const options& o) {
    const ai::tokenizer tok(g);
    const ai::chat ch(g, tok.token(tok.eos()));

    typename ai::model<T>::config c = ai::model<T>::config::from(g);

    ai::model<T> m(b, c);

    // Loaded before curses starts, so the progress and any failure land on a
    // normal terminal rather than inside a window that is about to be torn down.
    std::cerr << "[" << b.name() << ", " << c.layers << " layers, "
              << c.d_model << " wide]\n[loading]\n";

    const double t0 = now();

    m.load(g);

    const double took = now() - t0;

    m.enable_cache();

    screen s;

    s.say_line("jlib " + std::string(b.name()) + " -- " +
               std::to_string(c.layers) + " layers, loaded in " +
               std::to_string(took).substr(0, 4) + "s", note);
    s.say_line("Escape stops a reply.  PageUp scrolls back.  Ctrl-D quits.",
               note);
    s.say_line("");

    editor input;

    ai::sampler sampler(o.sampling);

    std::vector<ai::message> turns;

    if(!o.system.empty()) turns.push_back({ "system", o.system });

    for(;;) {
        std::string line;

        if(!input.read(s, line)) break;

        if(line.empty()) continue;

        // Into the transcript, and out of the input window: the prompt should
        // be empty while the reply arrives, or what was asked appears twice --
        // once where it was typed and once where it now belongs.
        s.say_line("> " + line, you);
        s.prompt("", 0);

        turns.push_back({ "user", line });

        // Room to keep free for the answer -- a floor, not a ceiling.  The
        // reply is allowed everything the context has left (below); this is
        // only how much of that room the trimmer will give up history to
        // guarantee.  Asking for more than the context can hold is therefore
        // harmless: the trimmer drops what it may and stops, rather than
        // deleting the question to satisfy a reservation nothing can meet.
        const std::size_t want = o.tokens;

        // Drop the oldest turns if the conversation has outgrown the context,
        // keeping the system prompt, which is instructions rather than history.
        for(;;) {
            // A model file that declares no context gives nothing to fit
            // into; without this the comparison below is against zero and the
            // loop drops every turn it is allowed to, every time.
            if(!c.context) break;

            if(ch.encode(turns, tok).size() + want <= c.context) break;

            // i + 1 < size, so the newest turn is never a candidate.  It is
            // the question, and a question deleted to make room for its own
            // answer leaves the model continuing a conversation that does not
            // contain it -- which it does fluently, and about something else.
            std::size_t at = turns.size();

            for(std::size_t i = 0; i + 1 < turns.size(); i++)
                if(turns[i].role != "system") { at = i; break; }

            if(at == turns.size()) break;       // no history left to give

            turns.erase(turns.begin() + long(at));
        }

        std::string reply;
        bool first = true;
        bool stopped = false;

        const double began = now();

        const std::vector<int> ids = ch.encode(turns, tok);

        // Whatever the context has left, all of it.  There is no application
        // cap on a reply: the model stops when it has finished, or when the
        // context is full, and Escape stops it in between.  That interrupt is
        // what makes a cap unnecessary rather than merely unhelpful -- it was
        // never buying safety, only truncating answers that had room to run.
        //
        // The arithmetic lands exactly.  generate() breaks when ids.size()
        // exceeds the context, checked at the top of each step, so a budget of
        // context - prompt runs out at the step where the sequence is one
        // short of full: the guard never fires and nothing overruns.
        const unsigned int budget = c.context
            ? static_cast<unsigned int>(c.context > ids.size()
                                        ? c.context - ids.size() : 0)
            : o.tokens;

        // The trimmer is best-effort, and stops when there is no more history
        // to give -- which is exactly when the system prompt and the question
        // alone leave no room to answer.  Say so.  Answering with the tokens
        // that remain produces a fragment; answering without the question,
        // which is what this used to do, produces a fluent reply to nothing.
        if(budget < MIN_REPLY) {
            s.say_line("[that prompt is " + std::to_string(ids.size()) +
                       " tokens and the context is " + std::to_string(c.context) +
                       " -- too long to answer.  Try a shorter question.]", note);
            s.say_line("");

            turns.pop_back();

            continue;
        }

        const std::vector<int> out = ai::generate<T>(
            m, b, ids, budget, sampler, tok.eos(),
            [&](int id) {
                std::string p = tok.piece(id);

                if(first) {
                    while(!p.empty() && p[0] == ' ') p.erase(0, 1);

                    first = false;
                }

                reply += p;

                s.say(p);

                // Between tokens, which is where stopping is safe and where a
                // keystroke can be looked for without blocking.  Scrolling back
                // mid-reply works for the same reason.
                const int key = s.key();

                if(key == KEY_BARE_ESC || g_interrupted) {
                    stopped = true;

                    return false;
                }

                if(key == KEY_RESIZE) s.layout();
                else if(key == KEY_PPAGE) s.scroll_by(s.view_rows() / 2);
                else if(key == KEY_NPAGE) s.scroll_by(-(s.view_rows() / 2));

                return true;
            });

        g_interrupted = 0;

        const double rate = (now() - began) > 0
            ? double(out.size() - ids.size()) / (now() - began) : 0.0;

        // How full the context is after this turn.  out holds the prompt as
        // well as what was generated, so its size is what the conversation now
        // occupies -- and roughly what the next turn starts from, since the
        // reply about to be appended is history like any other.
        //
        // Worth showing rather than computing silently: trimming and the
        // distance to the wall are otherwise invisible, and a reply cut short
        // by a full context reads as the model being bad at the question
        // rather than as the context being spent.  A model file that declares
        // no context length leaves nothing to compare against, so the figure
        // is omitted rather than guessed at.
        const std::string room = c.context
            ? ", " + std::to_string(out.size()) + "/" +
              std::to_string(c.context) + " context"
            : std::string();

        s.say_line("");
        s.say_line(std::string("[") + std::to_string(out.size() - ids.size()) +
                   " tokens, " + std::to_string(rate).substr(0, 5) + "/s" + room +
                   (stopped ? ", stopped]" : "]"), note);
        s.say_line("");

        turns.push_back({ "assistant", reply });
    }

    return 0;
}

}

int main(int argc, char** argv) {
    options o;

    if(!parse(argc, argv, o)) return 2;

    struct sigaction sa;

    std::memset(&sa, 0, sizeof(sa));

    sa.sa_handler = interrupted;

    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, 0);

    try {
        const ai::gguf g(o.model);

#ifdef HAVE_METAL
        try {
            jlib::metal::backend<_Float16> b;

            return converse<_Float16>(b, g, o);
        }
        catch(std::exception& e) {
            std::cerr << "jalpaca: no Metal backend (" << e.what() << ")\n";
        }
#endif

        std::cerr << "jalpaca: running on the CPU in float -- this will be very "
                  << "slow and will want about 4GB\n";

        ai::host_backend<float> b;

        return converse<float>(b, g, o);
    }
    catch(std::exception& e) {
        // endwin() has already run if a screen was up: it is a destructor.
        std::cerr << "jalpaca: " << e.what() << "\n";

        return 1;
    }
}
