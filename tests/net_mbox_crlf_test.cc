/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */

// An mbox whose lines end CRLF.
//
// MFolderBuffer::scan_headers looked for "\n\n" to find where a message's
// headers stopped.  A CRLF mailbox has "\r\n\r\n" there and no "\n\n"
// anywhere, so the search always failed -- and the "did we find it" guard was
// written against an `unsigned int`, which truncates npos to 0xFFFFFFFF and is
// therefore never equal to the real npos.  The guard could not fire, and
// buf.erase(0xFFFFFFFF) threw out_of_range.  Every CRLF mbox threw.
//
// gtkmail opens mbox:// accounts through this path, so it was reachable by
// pointing the client at a mailbox written by anything that speaks CRLF.

#include <jlib/net/MFolder.hh>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

/** Two messages, with the line endings given. */
static void write_mbox(const std::string& path, const std::string& eol) {
    std::ofstream out(path.c_str(), std::ios::binary);

    out << "From joey@dbzero.com Mon Dec  3 15:01:45 2001" << eol
        << "From: joey@dbzero.com" << eol
        << "To: someone@example.com" << eol
        << "Subject: the first one" << eol
        << eol
        << "body of the first message" << eol
        << eol
        << "From joey@dbzero.com Tue Dec  4 09:12:00 2001" << eol
        << "From: joey@dbzero.com" << eol
        << "To: someone@example.com" << eol
        << "Subject: the second one" << eol
        << eol
        << "body of the second message" << eol;
}

static void scans(const std::string& what, const std::string& eol) {
    const std::string path = "mbox_crlf_test.mbox";

    write_mbox(path, eol);

    bool threw = false;
    std::string why;
    std::size_t found = 0;

    try {
        jlib::net::MFolder folder(path);
        folder.scan();
        found = folder.size();
    }
    catch(std::exception& e) {
        threw = true;
        why = e.what();
    }

    ok(what + ": scanning does not throw", !threw, why);
    ok(what + ": both messages are found", found == 2, std::to_string(found));

    std::remove(path.c_str());
}

int main() {
    std::cout << std::unitbuf;

    std::cout << "an mbox, by line ending:\n";

    // LF has always worked; it is here so a regression in the shared path
    // cannot hide behind the CRLF fix.
    scans("LF  ", "\n");
    scans("CRLF", "\r\n");

    // What this does not establish: that CRLF mailboxes are handled correctly
    // throughout, only that scanning one no longer throws and finds the right
    // number of messages.  net::parse_divide still splits on any body line
    // beginning with "From ", and nothing here writes a mailbox, so the
    // >From-escaping that jlib does not do is untested and still absent.
    return failures ? 1 : 0;
}
