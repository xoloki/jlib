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

// jlib::util::json, which had no test at all.
//
// It has been in the tree since 2010 and the neural-net apps have been reading
// their weights through it for as long, so it works for the input it was
// written against: a file this program wrote itself.  What it had never seen
// is input from somewhere else, and the next caller is an OAuth2 token
// endpoint -- a stranger, answering over the network, whose error responses
// are the normal case rather than the exceptional one.
//
// Every section below is a thing that endpoint does on an ordinary day.  The
// first one used to be a segmentation fault.

#include <jlib/util/json.hh>

#include <iostream>
#include <string>

namespace json = jlib::util::json;

static int failures = 0;

static void ok(const std::string& what, bool good, const std::string& detail = "") {
    if(!good) ++failures;
    std::cout << (good ? "  ok   " : "  FAIL ") << what;
    if(!detail.empty()) std::cout << ": " << detail;
    std::cout << "\n";
}

static void a_key_that_is_not_there() {
    std::cout << "\na key that is not there:\n";

    // The body of a failed token request.  There is no access_token in it --
    // that is the whole message -- and reading one used to hand json-c's null
    // to std::string's constructor and take the process down.  A client cannot
    // check for the key first, because until has() there was nothing to check
    // with: proxy's operator bool answered "is it there", but only after the
    // conversion that crashed.
    json::object::ptr o = json::object::create("{\"error\":\"invalid_grant\"}");

    ok("the error is readable", std::string(o->get("error")) == "invalid_grant");

    ok("has() says the key is not there", !o->has("access_token"));
    ok("and present() agrees", !o->get("access_token").present());

    bool threw = false;
    std::string message;

    try {
        std::string token = o->get("access_token");

        ok("reading it throws rather than crashing", false, "got '" + token + "'");
    }
    catch(json::missing_key& e) {
        threw = true;
        message = e.what();
    }

    ok("reading it throws rather than crashing", threw);
    ok("and the message names the key",
       message.find("access_token") != std::string::npos, message);

    // The other half of the same problem: json-c stores a JSON null as a null
    // pointer, so an explicit null and an absent key are the same value.  Only
    // has() can tell them apart.
    json::object::ptr n = json::object::create("{\"refresh_token\":null}");

    ok("an explicit null is distinguishable from an absent key",
       n->has("refresh_token") && !n->has("nothing"));

    ok("but reading it still throws", [&n] {
        try { std::string s = n->get("refresh_token"); return false; }
        catch(json::missing_key&) { return true; }
    }());

    // And the accessor for a caller that would rather branch than catch.
    ok("str_or gives the default", o->get("access_token").str_or("none") == "none");
    ok("and the value when there is one", o->get("error").str_or("none") == "invalid_grant");
}

static void numbers_as_a_server_writes_them() {
    std::cout << "\nnumbers, as a server actually writes them:\n";

    // Microsoft's v1 token endpoint returns expires_in as a *string*.  Google's
    // returns it as a number.  A JSON number written 3599.0 is a double.  The
    // old code demanded json_type_int exactly and threw on two of the three.
    json::object::ptr o = json::object::create(
        "{\"a\":3599,\"b\":\"3599\",\"c\":3599.0,\"d\":3599.5,\"e\":\"soon\"}");

    ok("an integer reads as an integer", static_cast<int>(o->get("a")) == 3599);
    ok("a string of digits reads as an integer", static_cast<int>(o->get("b")) == 3599);
    ok("a whole double reads as an integer", static_cast<int>(o->get("c")) == 3599);

    // Where the coercion would lose something, it does not happen silently.
    ok("a fractional double does not", [&o] {
        try { int x = o->get("d"); (void)x; return false; }
        catch(json::type_mismatch&) { return true; }
    }());

    // json-c's own json_object_get_int64() coerces a string and returns 0 when
    // it cannot, so "soon" and "0" would be the same answer -- and an
    // expires_in that failed to parse becomes a token that expired in 1970.
    ok("and a word that is not a number does not read as zero", [&o] {
        try { int x = o->get("e"); (void)x; return false; }
        catch(json::type_mismatch&) { return true; }
    }());

    ok("an integer reads as a double", static_cast<double>(o->get("a")) == 3599.0);
    ok("and a double reads as a double", static_cast<double>(o->get("d")) == 3599.5);

    // Ranges.  These used to truncate through int without a word.
    json::object::ptr big = json::object::create(
        "{\"big\":4294967296,\"neg\":-1}");

    ok("a value too large for an int throws", [&big] {
        try { int x = big->get("big"); (void)x; return false; }
        catch(json::type_mismatch&) { return true; }
    }());

    ok("and reads fine as an int64",
       static_cast<int64_t>(big->get("big")) == 4294967296LL);

    // A negative read as a size_t is not a small number, it is an enormous
    // one, and it is usually about to be a length.
    ok("a negative read as a size throws", [&big] {
        try { std::size_t x = big->get("neg"); (void)x; return false; }
        catch(json::type_mismatch&) { return true; }
    }());

    ok("int_or gives the default for something unreadable",
       o->get("e").int_or(-1) == -1 && o->get("a").int_or(-1) == 3599);
}

static void a_boolean_means_the_value() {
    std::cout << "\na boolean means the value, not whether the key is there:\n";

    json::object::ptr o = json::object::create("{\"yes\":true,\"no\":false}");

    ok("true is true", static_cast<bool>(o->get("yes")));

    // This is the one that was wrong: operator bool returned m_obj != 0, so it
    // answered "is the key present" and {"no":false} read as true.  Anything
    // that branched on a JSON boolean took the wrong branch, every time,
    // whenever the answer was false.
    ok("and false is false", !static_cast<bool>(o->get("no")));

    ok("a missing key is false", !static_cast<bool>(o->get("neither")));
    ok("and present() is what asks the old question",
       o->get("no").present() && !o->get("neither").present());
}

static void the_whole_input_has_to_be_json() {
    std::cout << "\nthe whole input has to be JSON:\n";

    // json_tokener_parse() stops at the first complete value and says nothing
    // about the rest, so a body with two objects in it parsed as the first and
    // the second went unmentioned.  For a token response that is a
    // response-splitting primitive: whatever appends the second object decides
    // nothing, and whatever appends the first decides everything.
    ok("two objects is an error", [] {
        try {
            json::object::ptr o = json::object::create(
                "{\"access_token\":\"a\"}{\"access_token\":\"b\"}");

            return false;
        }
        catch(json::parse_error&) { return true; }
    }());

    ok("and so is trailing rubbish", [] {
        try {
            json::object::ptr o = json::object::create("{\"a\":1} and then some");

            return false;
        }
        catch(json::parse_error&) { return true; }
    }());

    // Whitespace after a value is not rubbish; a server is entitled to a
    // trailing newline and refusing one would be its own bug.
    ok("but trailing whitespace is not", [] {
        try {
            json::object::ptr o = json::object::create("{\"a\":1}\r\n\r\n");

            return static_cast<int>(o->get("a")) == 1;
        }
        catch(std::exception&) { return false; }
    }());

    ok("a truncated object is an error", [] {
        try {
            json::object::ptr o = json::object::create("{\"a\":1");

            return false;
        }
        catch(json::parse_error&) { return true; }
    }());
}

static void an_exception_does_not_carry_the_payload() {
    std::cout << "\nan exception does not carry the payload:\n";

    // The message used to be "data did not parse to JSON: '" + data + "'".
    // The data, in the caller this branch exists for, is a bearer token; the
    // message goes to a log, which is a longer-lived and less careful place
    // than a process's memory.  The description and the offset are enough to
    // find the fault.
    const std::string secret = "{\"access_token\":\"ya29.SUPERSECRET\" ";

    std::string message;

    try {
        json::object::ptr o = json::object::create(secret);
    }
    catch(std::exception& e) {
        message = e.what();
    }

    ok("it threw", !message.empty());
    ok("and the token is not in the message",
       message.find("SUPERSECRET") == std::string::npos, message);
    ok("but it says where and why", message.find("byte") != std::string::npos, message);

    // A type_mismatch used to be a bare std::exception, whose what() is the
    // string "std::exception" -- which breaks the house rule and tells a
    // caller nothing at all.
    json::object::ptr o = json::object::create("{\"n\":\"words\"}");

    std::string mismatch;

    try {
        int x = o->get("n");
        (void)x;
    }
    catch(std::exception& e) {
        mismatch = e.what();
    }

    ok("a type mismatch says what was wrong",
       mismatch.find("\"n\"") != std::string::npos &&
       mismatch.find("string") != std::string::npos, mismatch);
}

static void what_it_could_already_do_it_still_does() {
    std::cout << "\nwhat it could already do, it still does:\n";

    // The neural nets read their weights through this, so the building and
    // round-tripping half has to keep working exactly as it did.
    json::object::ptr o = json::object::create();

    o->add("name", std::string("net"));
    o->add("layers", 3);
    o->add("rate", 0.25);

    json::array::ptr a = json::array::create();

    a->add(1);
    a->add(2);
    a->add(3);

    o->add("hidden", a);

    json::object::ptr back = json::object::create(o->str());

    ok("a string round trips", std::string(back->get("name")) == "net");
    ok("an int round trips", static_cast<int>(back->get("layers")) == 3);
    ok("a double round trips", static_cast<double>(back->get("rate")) == 0.25);
    ok("an object knows it is one", back->is(json::object::type_object));

    json::array::ptr h = json::array::create(back->obj("hidden")->str());

    ok("an array round trips", h->size() == 3 &&
       static_cast<int>(h->get(0)) == 1 && static_cast<int>(h->get(2)) == 3);

    // size() on an object used to call json_object_array_length on it, which
    // is undefined for a non-array and asserts in some builds.
    ok("an object's size is its key count", back->size() == 4,
       std::to_string(back->size()));

    // An unsigned value over 2^31 went out through int and came back negative.
    json::object::ptr u = json::object::create();

    u->add("u", static_cast<unsigned int>(4000000000u));

    ok("a large unsigned survives the round trip",
       static_cast<int64_t>(json::object::create(u->str())->get("u")) == 4000000000LL,
       u->str());
}

int main() {
    std::cout << std::unitbuf;

    a_key_that_is_not_there();
    numbers_as_a_server_writes_them();
    a_boolean_means_the_value();
    the_whole_input_has_to_be_json();
    an_exception_does_not_carry_the_payload();
    what_it_could_already_do_it_still_does();

    // What a green run does not establish.
    //
    // Not that the facade is safe to use on hostile input generally.  It is
    // json-c underneath and json-c is the thing that has been audited; what is
    // tested here is the thin layer above it, which is where all of the faults
    // were.  Nothing here feeds it a deeply nested document, a hundred
    // megabytes, or invalid UTF-8, and nothing here sets a depth limit --
    // json-c has one, JSON_TOKENER_DEFAULT_DEPTH, and this code does not
    // choose it.
    //
    // Not the ownership rules.  object::add(ptr) transfers the underlying
    // json_object and clears the child's m_put, so using a child after adding
    // it to a parent is a question with an answer nobody has written down, and
    // no assertion here asks it.
    std::cout << "\n" << (failures ? "FAILED" : "PASSED") << ": " << failures
              << " failure(s)\n";

    return failures ? 1 : 0;
}
