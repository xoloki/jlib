/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2010 Joey Yandle <xoloki@gmail.com>
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

#include <json.h>

#include "json.hh"

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace jlib {
namespace util {
namespace json {

namespace {

    /** What json-c calls a type, for a message a person has to read. */
    const char* name_of(json_object* o) {
        if(o == 0) return "null";

        switch(json_object_get_type(o)) {
        case json_type_null:    return "null";
        case json_type_boolean: return "a boolean";
        case json_type_double:  return "a double";
        case json_type_int:     return "an integer";
        case json_type_object:  return "an object";
        case json_type_array:   return "an array";
        case json_type_string:  return "a string";
        }

        return "something else";
    }

    /**
     * A whole string as an integer, or nothing.
     *
     * json-c's own json_object_get_int64() will coerce a string for you and
     * returns 0 when it cannot -- so "abc" and "0" are the same answer, and an
     * expires_in that failed to parse becomes a token that expired in 1970.
     * strtoll with the end checked is the difference.
     */
    bool string_to_int64(const char* s, int64_t& out) {
        if(s == 0 || *s == 0) return false;

        errno = 0;

        char* end = 0;
        const long long v = std::strtoll(s, &end, 10);

        if(errno == ERANGE) return false;

        while(*end == ' ' || *end == '\t') end++;

        if(*end != 0) return false;

        out = v;

        return true;
    }

    bool string_to_double(const char* s, double& out) {
        if(s == 0 || *s == 0) return false;

        errno = 0;

        char* end = 0;
        const double v = std::strtod(s, &end);

        if(errno == ERANGE) return false;

        while(*end == ' ' || *end == '\t') end++;

        if(*end != 0) return false;

        out = v;

        return true;
    }

    bool to_int64(json_object* o, int64_t& out) {
        switch(json_object_get_type(o)) {
        case json_type_int:
            out = json_object_get_int64(o);
            return true;

        case json_type_boolean:
            out = json_object_get_boolean(o) ? 1 : 0;
            return true;

        case json_type_double: {
            const double d = json_object_get_double(o);

            // 3599.0 is the same number as 3599 and 3599.5 is not.  Truncating
            // silently is how a count of things becomes the wrong count.
            if(d != static_cast<double>(static_cast<int64_t>(d))) return false;

            out = static_cast<int64_t>(d);

            return true;
        }

        case json_type_string:
            return string_to_int64(json_object_get_string(o), out);

        default:
            return false;
        }
    }

    bool to_double(json_object* o, double& out) {
        switch(json_object_get_type(o)) {
        case json_type_double:
            out = json_object_get_double(o);
            return true;

        case json_type_int:
            out = static_cast<double>(json_object_get_int64(o));
            return true;

        case json_type_boolean:
            out = json_object_get_boolean(o) ? 1.0 : 0.0;
            return true;

        case json_type_string:
            return string_to_double(json_object_get_string(o), out);

        default:
            return false;
        }
    }

    /**
     * Parse, and insist that the whole input was JSON.
     *
     * json_tokener_parse() stops at the first complete value and says nothing
     * about what follows, so {"access_token":"a"}{"access_token":"b"} parsed
     * as the first of the two and `{"a":1} not json at all` parsed as {"a":1}.
     * For a token response that is a response-splitting primitive wearing a
     * convenience's clothes.
     */
    json_object* parse_whole(const std::string& data) {
        json_tokener* tok = json_tokener_new();

        if(tok == 0) throw parse_error("out of memory");

        json_object* o = json_tokener_parse_ex(tok, data.data(),
                                               static_cast<int>(data.size()));

        const json_tokener_error err = json_tokener_get_error(tok);
        const std::size_t end = json_tokener_get_parse_end(tok);

        json_tokener_free(tok);

        // The data is deliberately not in the message.  The first caller of
        // this is an OAuth2 token endpoint, whose body is a bearer token, and
        // an exception message goes to a log.  What is here is enough to find
        // the fault -- the tokener's own description, where it stopped, and
        // how long the input was -- and JLIB_UTIL_JSON_DEBUG adds the rest for
        // someone debugging a config file.
        std::ostringstream why;

        if(o == 0 || err != json_tokener_success) {
            if(o != 0) json_object_put(o);

            // json_tokener_continue means "give me more input", which is what
            // a tokener fed a whole document says when the document stops in
            // the middle of a value.  Its error_desc for that is the word
            // "continue", which describes the tokener's state and not the
            // caller's problem.
            if(err == json_tokener_continue || err == json_tokener_success) {
                why << "the input ends in the middle of a value";
            }
            else {
                why << json_tokener_error_desc(err);
            }

            why << ", at byte " << end << " of " << data.size();

            if(std::getenv("JLIB_UTIL_JSON_DEBUG")) why << ": '" << data << "'";

            throw parse_error(why.str());
        }

        std::size_t rest = end;

        while(rest < data.size() &&
              (data[rest] == ' ' || data[rest] == '\t' ||
               data[rest] == '\r' || data[rest] == '\n'))
            rest++;

        if(rest < data.size()) {
            json_object_put(o);

            why << "a complete value ended at byte " << end << " and "
                << (data.size() - rest) << " more octets follow it";

            if(std::getenv("JLIB_UTIL_JSON_DEBUG")) why << ": '" << data << "'";

            throw parse_error(why.str());
        }

        return o;
    }

}

proxy::proxy(json_object* obj, const std::string& what)
    : m_obj(obj),
      m_what(what)
{}

void proxy::require() const {
    if(m_obj == 0) {
        throw missing_key(m_what.empty()
                          ? std::string("no value")
                          : ("no value for " + m_what));
    }
}

proxy::operator std::string() const {
    require();

    // Permissive on purpose: a number, a boolean, or a whole nested object
    // comes back as the JSON text of it, which is what json-c does and is
    // occasionally what a caller wants.  What is not permitted is the null
    // that used to be handed straight to std::string's constructor.
    const char* s = json_object_get_string(m_obj);

    if(s == 0) throw type_mismatch(m_what + " is " + name_of(m_obj));

    return s;
}

proxy::operator int64_t() const {
    require();

    int64_t v = 0;

    if(!to_int64(m_obj, v))
        throw type_mismatch(m_what + " is " + name_of(m_obj) + ", not an integer");

    return v;
}

proxy::operator int() const {
    const int64_t v = operator int64_t();

    if(v < INT_MIN || v > INT_MAX)
        throw type_mismatch(m_what + " does not fit in an int");

    return static_cast<int>(v);
}

proxy::operator unsigned int() const {
    const int64_t v = operator int64_t();

    if(v < 0 || v > static_cast<int64_t>(UINT_MAX))
        throw type_mismatch(m_what + " does not fit in an unsigned int");

    return static_cast<unsigned int>(v);
}

proxy::operator std::size_t() const {
    const int64_t v = operator int64_t();

    // A negative read as a size_t is not a small number, it is an enormous
    // one, and it is usually about to be a length.
    if(v < 0) throw type_mismatch(m_what + " is negative and was read as a size");

    return static_cast<std::size_t>(v);
}

proxy::operator double() const {
    require();

    double v = 0;

    if(!to_double(m_obj, v))
        throw type_mismatch(m_what + " is " + name_of(m_obj) + ", not a number");

    return v;
}

proxy::operator float() const {
    return static_cast<float>(operator double());
}

proxy::operator long double() const {
    return operator double();
}

proxy::operator bool() const {
    // This used to return m_obj != 0 -- so it answered "is the key there?"
    // rather than "what is the value?", and {"admin":false} read as true.  It
    // is the JSON boolean now, coerced as json-c coerces: 0, "", [] and {} are
    // false and everything else is true.  present() is the old question, asked
    // in a way that says what it means.
    if(m_obj == 0) return false;

    return json_object_get_boolean(m_obj) != 0;
}

std::string proxy::str_or(const std::string& dflt) const {
    if(m_obj == 0) return dflt;

    const char* s = json_object_get_string(m_obj);

    return s == 0 ? dflt : std::string(s);
}

int64_t proxy::int_or(int64_t dflt) const {
    int64_t v = 0;

    if(m_obj == 0 || !to_int64(m_obj, v)) return dflt;

    return v;
}

double proxy::double_or(double dflt) const {
    double v = 0;

    if(m_obj == 0 || !to_double(m_obj, v)) return dflt;

    return v;
}

bool proxy::bool_or(bool dflt) const {
    if(m_obj == 0) return dflt;

    return json_object_get_boolean(m_obj) != 0;
}

object::ptr object::create() { 
    return ptr(new object());
}
    
object::ptr object::create(const std::string& data) { 
    return ptr(new object(data));
}
    
object::object() 
    : m_obj(json_object_new_object()),
      m_put(true)
{}
    
object::object(const std::string& data)
    : m_obj(parse_whole(data)),
      m_put(true)
{}
    
object::object(json_object* obj, bool put) 
    : m_obj(obj),
      m_put(put)
{
}
    
object::~object() { 
    if(m_put)
	json_object_put(m_obj); 
}
    
proxy object::get(const std::string& key) const {
    json_object* v = 0;

    // _ex, because json-c stores a JSON null as a null pointer: without it
    // "the key is not there" and "the key is there and is null" are the same
    // answer, and the error message cannot tell the caller which it was.
    const bool there = json_object_object_get_ex(m_obj, key.c_str(), &v);

    std::string what = "\"" + key + "\"";

    if(there && v == 0) what += ", which is null";

    return proxy(v, what);
}

bool object::has(const std::string& key) const {
    return json_object_object_get_ex(m_obj, key.c_str(), 0) != 0;
}

proxy object::get(std::size_t idx) const {
    std::ostringstream o; o << "index " << idx;

    return proxy(json_object_array_get_idx(m_obj, idx), o.str());
}
    
void object::add(const std::string& key, const std::string& val) {
    json_object_object_add(m_obj, key.data(), json_object_new_string(const_cast<char*>(val.data())));
}
    
void object::add(const std::string& key, int val) {
    json_object_object_add(m_obj, key.data(), json_object_new_int(val));
}
    
void object::add(const std::string& key, unsigned int val) {
    // Through int, this turned anything over 2^31 negative on the way out.
    json_object_object_add(m_obj, key.data(),
                           json_object_new_int64(static_cast<int64_t>(val)));
}
    
void object::add(const std::string& key, double val) {
    json_object_object_add(m_obj, key.data(), json_object_new_double(val));
}
    
void object::add(const std::string& key, long double val) {
    add(key, static_cast<double>(val));
}
    
void object::add(const std::string& key, object::ptr val) {
    json_object_object_add(m_obj, key.data(), val->obj());
    val->m_put = false;
}
    
void object::add(const std::string& key, object::arrayptr val) {
    json_object_object_add(m_obj, key.data(), val->obj());
    val->m_put = false;
}
    
std::string object::str(bool pretty) const {
    std::string ret(json_object_to_json_string_ext(m_obj, pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_SPACED));
    
    return ret;
}
    
json_object* object::obj() {
    return m_obj;
}
    
object::ptr object::obj(const std::string& key) {
    json_object* o = 0;

    if(!json_object_object_get_ex(m_obj, key.c_str(), &o) || o == 0)
        throw missing_key("no object for \"" + key + "\"");

    return ptr(new object(o, false));
}
    
object::ptr object::obj(unsigned int x) {
    json_object* o = json_object_array_get_idx(m_obj, x);
    return ptr(new object(o, false));
}
    
std::size_t object::size() const {
    // An object is not an array, and asking json-c for the array length of one
    // is undefined -- some builds assert.  An object's size is its key count.
    if(json_object_is_type(m_obj, json_type_array)) return json_object_array_length(m_obj);
    if(json_object_is_type(m_obj, json_type_object)) return json_object_object_length(m_obj);

    return 0;
}
    
bool object::is(object::type t) const {
    return json_object_is_type(m_obj, (json_type)(int)t);
}
    
array::ptr array::create() { 
    return ptr(new array());
}
    
array::ptr array::create(const std::string& data) { 
    return ptr(new array(data));
}
    
array::array() 
    : m_obj(json_object_new_array()),
      m_put(true)
{}
    
array::array(const std::string& data)
    : m_obj(parse_whole(data)),
      m_put(true)
{}
    
array::array(json_object* obj) 
    : m_obj(obj),
      m_put(false)
{
}
    
array::~array() { 
    if(m_put)
        json_object_put(m_obj); 
}
    
void array::add(const std::string& val) {
    json_object_array_add(m_obj, json_object_new_string(const_cast<char*>(val.data())));
}
    
void array::add(int val) {
    json_object_array_add(m_obj, json_object_new_int(val));
}
    
void array::add(unsigned int val) {
    json_object_array_add(m_obj, json_object_new_int64(static_cast<int64_t>(val)));
}
    
void array::add(double val) {
    json_object_array_add(m_obj, json_object_new_double(val));
}
    
void array::add(long double val) {
    add(static_cast<double>(val));
}
    
void array::add(object::ptr val) {
    json_object_array_add(m_obj, val->obj());
    val->m_put = false;
}
    
void array::add(array::ptr val) {
    json_object_array_add(m_obj, val->obj());
    val->m_put = false;
}
    
proxy array::get(unsigned int x) const {
    std::ostringstream o; o << "index " << x;

    return proxy(json_object_array_get_idx(m_obj, x), o.str());
}

object::ptr array::obj(unsigned int x) const {
    return object::ptr(new object(json_object_array_get_idx(m_obj, x), false));
}
    
array::ptr array::arr(unsigned int x) const {
    return ptr(new array(json_object_array_get_idx(m_obj, x)));
}
    
int array::size() const {
    return json_object_array_length(m_obj);
}
    
json_object* array::obj() {
    return m_obj;
}

std::string array::str() const {
    std::string ret(json_object_to_json_string(m_obj));
    
    return ret;
}
    
}
}
}
