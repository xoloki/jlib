/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2002 Joey Yandle <xoloki@gmail.com>
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
/*
 * util_headers_set_test -- set() collapsing a key that add() duplicated.
 *
 * `add()` pushes its key onto the ordered key list every time, so adding the
 * same field twice puts it in that list twice -- which is right, because two
 * adds are two fields.  `set()` replaces the values with one value, and so has
 * to collapse the key list to match, or the field is written out once per
 * entry that is still in there.
 *
 * It did not.  The line meant to do it called std::remove and threw the result
 * away -- std::remove shifts and returns the new end, it does not shorten a
 * container -- and searched for the caller's `key` rather than the uppercased
 * `k` it had just computed, so it could not have matched even if the result
 * had been used.  Found by Xcode 27 warning about the discarded [[nodiscard]],
 * on code that had been there since long before.
 *
 * **The blast radius is smaller than it looks**, and worth stating so nobody
 * reads this as a wire-format bug.  Serialisation walks the key list and emits
 * the nth value for the nth sighting of a key; once the values are exhausted
 * it emits nothing.  So a stale key entry wrote no stale field.  What it did
 * was make keys() report a field that is no longer there twice -- which is
 * wrong, and is all it is.
 */

#include <jlib/util/Headers.hh>

#include <iostream>
#include <string>

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static std::size_t times(const jlib::util::Headers::list_type& l,
                         const std::string& want)
{
    std::size_t n = 0;

    for(jlib::util::Headers::list_type::const_iterator i = l.begin();
        i != l.end(); ++i) {
        if(*i == want) n++;
    }

    return n;
}

int main() {
    std::cout << "util_headers_set_test\n";

    {
        std::cout << "\ntwo adds put the key in the list twice, as they should:\n";

        jlib::util::Headers h;

        h.add("X-Thing", "one");
        h.add("X-Thing", "two");

        ok("  the key is listed twice", times(h.keys(), "X-THING") == 2,
           std::to_string(times(h.keys(), "X-THING")));

        ok("  and both values are there", h.vals("X-Thing").size() == 2,
           std::to_string(h.vals("X-Thing").size()));
    }

    {
        std::cout << "\nset() collapses both the values and the key list:\n";

        jlib::util::Headers h;

        h.add("X-Thing", "one");
        h.add("X-Thing", "two");
        h.set("X-Thing", "only");

        ok("  one value, because that is what set means",
           h.vals("X-Thing").size() == 1 &&
           h.vals("X-Thing").front() == "only",
           std::to_string(h.vals("X-Thing").size()));

        // **The assertion that was failing.**  The key list still held two
        // entries, so the field was written out twice with the same value.
        ok("  and the key is listed once, not once per add that preceded it",
           times(h.keys(), "X-THING") == 1,
           std::to_string(times(h.keys(), "X-THING")) + " entries");

        // **This one passes either way, and that is the finding.**  The
        // serialiser walks the key list and emits the *nth* value for the nth
        // time it sees a key; when the values run out it emits nothing.  So a
        // stale entry produced no stale field, which is why nothing noticed
        // for twenty-six years -- the damage is confined to keys().
        const std::string wire(h);

        std::size_t seen = 0;

        for(std::size_t at = wire.find("X-Thing"); at != std::string::npos;
            at = wire.find("X-Thing", at + 1)) {
            seen++;
        }

        ok("  the field still goes out once (true before the fix as well)",
           seen == 1, std::to_string(seen) + " in \"" +
           wire.substr(0, 40) + "\"");
    }

    {
        std::cout << "\nand a key set without ever being added is unaffected:\n";

        jlib::util::Headers h;

        h.set("X-Fresh", "value");

        ok("  listed once", times(h.keys(), "X-FRESH") == 1,
           std::to_string(times(h.keys(), "X-FRESH")));
    }

    if(failures) {
        std::cerr << "util_headers_set_test: " << failures << " failed\n";
        return 1;
    }

    std::cout << "util_headers_set_test: all good\n";

    return 0;
}
