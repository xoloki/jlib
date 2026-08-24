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

// same_address and is_addr: two predicates that used to say yes too readily.
//
// same_address extracted both addresses, uppercased, and compared.
// extract_address returns "" for anything with no @ in it, so two unparseable
// strings both became "" and compared equal: same_address("Joe Yandle",
// "Bob Smith") was true.  A function whose whole job is "is this the same
// person" answered yes for two different people whenever it could not read
// either one of them.
//
// is_addr looped over the string returning false for anything that was not a
// digit or a dot -- so it could only ever reject, never accept by mistake, and
// "" and "...." were both addresses.

#include <jlib/net/net.hh>

#include <iostream>
#include <string>

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static void an_address_it_cannot_read_matches_nothing() {
    std::cout << "same_address:\n";

    // The regression.  Both of these extract to "", and used to compare equal.
    ok("two different names are not the same person",
       !jlib::net::same_address("Joe Yandle", "Bob Smith"));

    ok("nor are an empty string and some garbage",
       !jlib::net::same_address("", "garbage"));

    ok("nor are two empty strings",
       !jlib::net::same_address("", ""));

    ok("nor is a name the same as a real address",
       !jlib::net::same_address("Joe Yandle", "joey@dbzero.com"));

    // and it still says yes when it should
    ok("the same address is the same address",
       jlib::net::same_address("joey@dbzero.com", "joey@dbzero.com"));

    ok("through a display name",
       jlib::net::same_address("Joey Yandle <joey@dbzero.com>", "joey@dbzero.com"));

    ok("and case does not matter",
       jlib::net::same_address("JOEY@DBZERO.COM", "joey@dbzero.com"));
}

static void only_a_dotted_quad_is_an_address() {
    std::cout << "\nis_addr:\n";

    // The regression: neither of these has any digits in it at all.
    ok("an empty string is not an address", !jlib::net::is_addr(""));
    ok("nor is a row of dots",              !jlib::net::is_addr("...."));

    ok("a dotted quad is",                   jlib::net::is_addr("192.168.0.1"));
    ok("and the edges of one",               jlib::net::is_addr("0.0.0.0"));
    ok("and the other edge",                 jlib::net::is_addr("255.255.255.255"));

    ok("three octets is not enough",        !jlib::net::is_addr("1.2.3"));
    ok("five is too many",                  !jlib::net::is_addr("1.2.3.4.5"));
    ok("an octet over 255 is not one",      !jlib::net::is_addr("1.2.3.256"));
    ok("nor is a four digit octet",         !jlib::net::is_addr("1.2.3.0001"));
    ok("a trailing dot is not an address",  !jlib::net::is_addr("1.2.3.4."));
    ok("nor a leading one",                 !jlib::net::is_addr(".1.2.3.4"));
    ok("a hostname is not an address",      !jlib::net::is_addr("dbzero.com"));
    ok("nor is a hostname of digits",       !jlib::net::is_addr("1.2.3.four"));
}

int main() {
    std::cout << std::unitbuf;

    an_address_it_cannot_read_matches_nothing();
    only_a_dotted_quad_is_an_address();

    // What this does not establish.
    //
    // same_address still folds case across the whole address, including the
    // local part, which RFC 5321 2.4 says is case sensitive.  That is a
    // deliberate hold rather than an oversight -- the requirement binds
    // relays, jlib is a user agent, and every large provider folds -- and it
    // is due to become a policy flag when the RFC 5322 parser lands.  Nothing
    // here tests the quoted local part ("Joe"@x.com vs "joe"@x.com), which is
    // the case where 5321 is unambiguous and folding is simply wrong.
    //
    // is_addr only knows IPv4.  is_addr("::1") is false, and get_host will
    // therefore not attempt a reverse lookup on an IPv6 literal.  That is
    // unchanged behaviour, not something this fix addresses.
    return failures ? 1 : 0;
}
