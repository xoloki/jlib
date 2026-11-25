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

#ifndef JLIB_TESTS_VT_HH
#define JLIB_TESTS_VT_HH

/**
 * Just enough terminal to answer "what is on the screen".
 *
 * ### Why this exists, having been argued against
 *
 * `pty.hh` says interpreting escape sequences is "writing a terminal
 * emulator, which is far more than a test harness should be", and takes the
 * other road: match the stream, and accept that the question answered is
 * "did the program ever paint this" rather than "is this visible".
 *
 * That was wrong, and twice over. Stream matching passed a deliberately
 * broken build once (a negative assertion read before the paint arrived) and
 * then passed a **genuinely** broken juanaco: the transcript was blitted from
 * a pad and immediately wiped by the following `refresh()`, so every slash
 * command painted its answer and then erased it. Nothing was visible but the
 * header, the prompt and the status line. Every assertion in the suite passed,
 * on both platforms, and the bug was found by a human looking at the screen.
 *
 * The distinction between painted and visible is not a refinement, then. It is
 * the difference between working and not, so the harness has to model it.
 *
 * ### What it models, which is only what curses emits here
 *
 * `CUP` (`ESC[r;cH`), `VPA` (`ESC[rd`), the four one-axis moves
 * (`ESC[nA/B/C/D`), `EL` (`ESC[K`, `ESC[0K`, `ESC[1K`, `ESC[2K`), `ED`
 * (`ESC[J`, `ESC[2J`), and `CR`, `LF` and `BS`. Every other CSI -- colour,
 * scroll regions, cursor visibility -- is parsed and discarded, and the two
 * character-set sequences (`ESC(B`) are skipped.
 *
 * ### What it does not model, and where that will bite
 *
 * - **Scroll regions.** `ESC[r;sr` is discarded, so a program that sets one
 *   and relies on `LF` scrolling inside it will be modelled wrongly. ncurses
 *   does set one; it does not depend on it for the drawing asserted here.
 * - **Line wrap at the right margin.** A write past the last column is
 *   truncated rather than wrapped.
 * - **Width.** One byte is one cell, so a multi-byte character occupies as
 *   many cells as it has bytes and a wide character is counted as one. Enough
 *   for ASCII assertions and wrong for anything else.
 * - **Alternate screens, insert/delete line, tabs.**
 *
 * It is a diagnostic instrument, not a terminal. When an assertion disagrees
 * with a real terminal, this file is the thing to doubt first.
 */

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace jlib {
namespace tests {

class vt {
public:
    vt(int rows, int cols)
        : m_rows(rows), m_cols(cols), m_cells(std::size_t(rows * cols), ' ')
    {}

    /** Apply bytes.  Can be called repeatedly as more arrive. */
    void feed(const std::string& bytes) {
        for(std::size_t i = 0; i < bytes.size(); i++) {
            const unsigned char c = static_cast<unsigned char>(bytes[i]);

            if(c == 27) { i = escape(bytes, i); continue; }

            if(c == '\r') { m_col = 0; continue; }
            if(c == '\n') { down(); m_col = 0; continue; }
            if(c == '\b') { if(m_col) m_col--; continue; }
            if(c == 7) continue;                        // bell

            if(c < 32) continue;                        // anything else control

            put(char(c));
        }
    }

    /** One row, trailing blanks removed. */
    std::string row(int r) const {
        if(r < 0 || r >= m_rows) return "";

        std::string out(m_cells.begin() + r * m_cols,
                        m_cells.begin() + (r + 1) * m_cols);

        while(!out.empty() && out.back() == ' ') out.pop_back();

        return out;
    }

    /** Every row, for a failure message that shows what was there instead. */
    std::string dump() const {
        std::string out;

        for(int r = 0; r < m_rows; r++) {
            out += std::to_string(r);
            out += r < 10 ? "  |" : " |";
            out += row(r);
            out += "\n";
        }

        return out;
    }

    /** The first row containing `text`, or -1.  Visible, not merely painted. */
    int find(const std::string& text) const {
        for(int r = 0; r < m_rows; r++)
            if(row(r).find(text) != std::string::npos) return r;

        return -1;
    }

    bool shows(const std::string& text) const { return find(text) >= 0; }

    int cursor_row() const { return m_row; }
    int cursor_col() const { return m_col; }

private:
    void put(char ch) {
        if(m_row >= 0 && m_row < m_rows && m_col >= 0 && m_col < m_cols)
            m_cells[std::size_t(m_row * m_cols + m_col)] = ch;

        if(m_col < m_cols - 1) m_col++;
    }

    void down() { if(m_row < m_rows - 1) m_row++; }

    void erase_line(int mode) {
        if(m_row < 0 || m_row >= m_rows) return;

        const int from = mode == 1 ? 0 : (mode == 2 ? 0 : m_col);
        const int to = mode == 1 ? m_col + 1 : m_cols;

        for(int c = from; c < to && c < m_cols; c++)
            m_cells[std::size_t(m_row * m_cols + c)] = ' ';
    }

    void erase_all() {
        m_cells.assign(std::size_t(m_rows * m_cols), ' ');
    }

    /** Consume one escape sequence; returns the index of its last byte. */
    std::size_t escape(const std::string& s, std::size_t at) {
        if(at + 1 >= s.size()) return at;

        const char kind = s[at + 1];

        // ESC(B and friends: a character set selection, two bytes of payload.
        if(kind == '(' || kind == ')' || kind == '*' || kind == '+')
            return at + 2 < s.size() ? at + 2 : s.size() - 1;

        if(kind != '[') return at + 1;                  // ESC something: skip

        // CSI: parameters, then a final byte in @..~
        std::size_t i = at + 2;
        std::string params;

        while(i < s.size() && !(s[i] >= '@' && s[i] <= '~')) {
            params += s[i];
            i++;
        }

        if(i >= s.size()) return s.size() - 1;          // incomplete; drop it

        const char final_byte = s[i];

        // A private sequence (ESC[?25l and the like) changes nothing here.
        if(!params.empty() && (params[0] == '?' || params[0] == '>'))
            return i;

        std::vector<int> n = numbers(params);

        switch(final_byte) {
        case 'H': case 'f':
            m_row = (n.size() > 0 && n[0] > 0 ? n[0] : 1) - 1;
            m_col = (n.size() > 1 && n[1] > 0 ? n[1] : 1) - 1;
            break;

        case 'd':
            m_row = (n.size() > 0 && n[0] > 0 ? n[0] : 1) - 1;
            break;

        case 'G':
            m_col = (n.size() > 0 && n[0] > 0 ? n[0] : 1) - 1;
            break;

        case 'A': m_row -= n.empty() || !n[0] ? 1 : n[0]; break;
        case 'B': m_row += n.empty() || !n[0] ? 1 : n[0]; break;
        case 'C': m_col += n.empty() || !n[0] ? 1 : n[0]; break;
        case 'D': m_col -= n.empty() || !n[0] ? 1 : n[0]; break;

        case 'K': erase_line(n.empty() ? 0 : n[0]); break;
        case 'J': erase_all(); break;

        default: break;                                 // SGR, scroll region…
        }

        if(m_row < 0) m_row = 0;
        if(m_col < 0) m_col = 0;
        if(m_row >= m_rows) m_row = m_rows - 1;
        if(m_col >= m_cols) m_col = m_cols - 1;

        return i;
    }

    static std::vector<int> numbers(const std::string& params) {
        std::vector<int> out;

        std::size_t i = 0;

        while(i < params.size()) {
            int v = 0;
            bool any = false;

            while(i < params.size() &&
                  std::isdigit(static_cast<unsigned char>(params[i]))) {
                v = v * 10 + (params[i] - '0');
                any = true;
                i++;
            }

            if(any) out.push_back(v);
            else if(params[i] == ';') out.push_back(0);

            while(i < params.size() && params[i] != ';') i++;

            if(i < params.size()) i++;
        }

        return out;
    }

    int m_rows;
    int m_cols;
    int m_row = 0;
    int m_col = 0;
    std::vector<char> m_cells;
};

} // namespace tests
} // namespace jlib

#endif // JLIB_TESTS_VT_HH
