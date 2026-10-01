/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 2011 Joey Yandle <xoloki@gmail.com>
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

#include <iostream>

#include <jlib/net/Email.hh>

int main(int argc, char** argv) {

    std::string raw = "Return-Path: <jwy@divisionbyzero.com>\n"
      "Received: 64.81.68.235\n"
      "Received: 192.168.0.1\n"
      "Received: 172.17.0.1\n"
      "Received: 10.0.0.1\n"
      "Date: Mon, 29 Oct 2001 00:59:52 -0800\n"
      "Message-Id: <200110290859.f9T8xj318352@devotchka.germtop.com>\n"
      "From: foo@bar.com\n"
      "Received: 64.81.68.242\n"
      "Subject: i hate you, so, very much\n"
      "Mime-Version: 1.0\n"
      "Content-Type: text/plain\n"
      "\n"
      "i hate you guys\n"
      "\n"
      "\n";

    int failures = 0;

    auto expect = [&failures](const std::string& text, const std::string& want,
                              const std::string& what) {
        jlib::net::Email email(text);
        const std::string got = email.get_received_ip();

        if(got == want) {
            std::cout << "  ok    " << what << "\n";
        } else {
            std::cout << "  FAIL  " << what << ": got [" << got
                      << "] want [" << want << "]\n";
            failures++;
        }
    };

    expect(raw, "64.81.68.235", "the originating hop, skipping reserved ranges");

    // The cases the POSIX regex this replaced got wrong.  It matched
    // ([[:digit:]]{1,3}\.){3}[[:digit:]]{1,3} in effect, which has no idea
    // what an octet is; RFC 3986's IPv4address does.
    expect("Received: 999.999.999.999\n\nbody\n", "",
           "999.999.999.999 is not an address");

    expect("Received: 256.1.1.1\n\nbody\n", "",
           "256 is not an octet");

    // Four dot-separated numbers inside a longer run.  The regex picked
    // "1.2.3.4" out of the front of this; the grammar rejects the run whole.
    expect("Received: 1.2.3.4.5\n\nbody\n", "",
           "a five-group run is not an address");

    // A timestamp the regex would have mined for a false address.
    expect("Received: from x; id 2026.10.01.12\n\nbody\n", "",
           "a dotted timestamp is not an address");

    // Still finds a real one next to other digits.
    expect("Received: from mx (mx [203.0.113.5]) by id 12345\n\nbody\n",
           "203.0.113.5", "a bracketed address beside other digits");

    // And still skips reserved ranges when that is all there is.
    expect("Received: 10.0.0.1\n\nbody\n", "",
           "a reserved address yields nothing");

    std::cout << (failures ? "FAILED" : "PASSED") << "\n";

    return failures ? 1 : 0;
}
