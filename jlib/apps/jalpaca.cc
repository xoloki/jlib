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
 * The screen: a transcript that scrolls and a prompt that does not.
 *
 * endwin() runs from the destructor, so a throw on the way out of main still
 * hands the terminal back. That is the whole reason this is a class.
 */
class screen {
public:
    screen() {
        initscr();
        cbreak();               // keys arrive unbuffered, which Escape needs
        noecho();               // the input window draws the line itself
        keypad(stdscr, TRUE);
        curs_set(1);

        layout();
    }

    ~screen() {
        endwin();
    }

    screen(const screen&) = delete;
    screen& operator=(const screen&) = delete;

    /** Rebuild both windows for the terminal's current size. */
    void layout() {
        int rows = 0, cols = 0;

        getmaxyx(stdscr, rows, cols);

        if(m_transcript) delwin(m_transcript);
        if(m_input) delwin(m_input);

        // Two rows at the bottom: one for the prompt, one for the rule above
        // it, so a long reply never runs into what is being typed.
        m_rows = rows;
        m_cols = cols;

        m_transcript = newwin(rows - 2, cols, 0, 0);
        m_input = newwin(1, cols, rows - 1, 0);

        scrollok(m_transcript, TRUE);
        idlok(m_transcript, TRUE);
        nodelay(m_input, TRUE);
        keypad(m_input, TRUE);

        redraw_rule();
    }

    void redraw_rule() {
        mvhline(m_rows - 2, 0, ACS_HLINE, m_cols);
        refresh();
    }

    /** Add text to the transcript, wrapping as the window does. */
    void say(const std::string& text) {
        waddstr(m_transcript, text.c_str());
        wrefresh(m_transcript);
    }

    void say_line(const std::string& text) { say(text + "\n"); }

    void prompt(const std::string& so_far) {
        werase(m_input);
        mvwaddstr(m_input, 0, 0, "> ");
        waddnstr(m_input, so_far.c_str(), m_cols - 3);
        wrefresh(m_input);
    }

    /** A keystroke if one is waiting, or ERR. Never blocks. */
    int poll_key() { return wgetch(m_input); }

    /**
     * Read a line, redrawing as it is typed.
     *
     * @return false at end of input or on a second interrupt
     */
    bool read_line(std::string& out) {
        out.clear();

        for(;;) {
            prompt(out);

            const int c = wgetch(m_input);

            if(c == ERR) {
                napms(20);      // nothing waiting; do not spin

                if(g_interrupted) { g_interrupted = 0; return false; }

                continue;
            }

            if(c == KEY_RESIZE) { layout(); continue; }

            if(c == '\n' || c == '\r') return true;

            if(c == 27) { out.clear(); continue; }          // Escape clears

            if(c == KEY_BACKSPACE || c == 127 || c == 8) {
                if(!out.empty()) out.erase(out.size() - 1);

                continue;
            }

            if(c == 4) return false;                        // Ctrl-D

            if(c >= 32 && c < 256) out += char(c);
        }
    }

private:
    WINDOW* m_transcript = 0;
    WINDOW* m_input = 0;

    int m_rows = 0;
    int m_cols = 0;
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
                      << "  reply; Ctrl-D or an empty Escape at the prompt quits.\n"
                      << "\n"
                      << "  --system TEXT   a system turn before the conversation\n"
                      << "  --tokens N      most tokens in one reply (default 512)\n"
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
               std::to_string(took).substr(0, 4) + "s");
    s.say_line("Escape stops a reply.  Ctrl-D quits.");
    s.say_line("");

    ai::sampler sampler(o.sampling);

    std::vector<ai::message> turns;

    if(!o.system.empty()) turns.push_back({ "system", o.system });

    for(;;) {
        std::string line;

        if(!s.read_line(line)) break;

        if(line.empty()) continue;

        // Into the transcript, and out of the input window: the prompt should
        // be empty while the reply arrives, or what was asked appears twice --
        // once where it was typed and once where it now belongs.
        s.say_line("> " + line);
        s.prompt("");

        turns.push_back({ "user", line });

        // Drop the oldest turns if the conversation has outgrown the context,
        // keeping the system prompt, which is instructions rather than history.
        for(;;) {
            if(ch.encode(turns, tok).size() + o.tokens <= c.context) break;

            std::size_t at = turns.size();

            for(std::size_t i = 0; i < turns.size(); i++)
                if(turns[i].role != "system") { at = i; break; }

            if(at == turns.size()) break;

            turns.erase(turns.begin() + long(at));
        }

        std::string reply;
        bool first = true;
        bool stopped = false;

        const double began = now();

        const std::vector<int> ids = ch.encode(turns, tok);

        const std::vector<int> out = ai::generate<T>(
            m, b, ids, o.tokens, sampler, tok.eos(),
            [&](int id) {
                std::string p = tok.piece(id);

                if(first) {
                    while(!p.empty() && p[0] == ' ') p.erase(0, 1);

                    first = false;
                }

                reply += p;

                s.say(p);

                // Between tokens, which is where stopping is safe and where a
                // keystroke can be looked for without blocking.
                const int key = s.poll_key();

                if(key == 27 || g_interrupted) { stopped = true; return false; }

                if(key == KEY_RESIZE) s.layout();

                return true;
            });

        g_interrupted = 0;

        const double rate = (now() - began) > 0
            ? double(out.size() - ids.size()) / (now() - began) : 0.0;

        s.say_line("");
        s.say_line(std::string("[") + std::to_string(out.size() - ids.size()) +
                   " tokens, " + std::to_string(rate).substr(0, 5) + "/s" +
                   (stopped ? ", stopped]" : "]"));
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
