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

#ifndef JLIB_JSON_HH
#define JLIB_JSON_HH

#include <string>
#include <exception>
#include <cstdint>

#include <memory>

struct json_object;

namespace jlib {
namespace util {
namespace json {

class array;

/**
 * Anything this facade throws.
 *
 * There was no such thing before: object::create() threw std::runtime_error
 * and proxy threw a type_mismatch whose what() was the default
 * "std::exception", so a caller catching one learned nothing about which key,
 * which type, or which byte.
 */
class exception : public std::exception {
public:
    exception(const std::string& msg = "") {
        m_msg = "json exception: " + msg;
    }
    virtual ~exception() {}
    virtual const char* what() const noexcept { return m_msg.c_str(); }
protected:
    std::string m_msg;
};

/** The value is there and is not the type that was asked for. */
class type_mismatch : public exception {
public:
    type_mismatch(const std::string& msg = "") : exception("type mismatch: " + msg) {}
};

/**
 * The value is not there at all, or is JSON null.
 *
 * This is the one that mattered.  A missing key produced a proxy wrapping a
 * null pointer, and reading it as a string handed that null to std::string's
 * constructor -- so the very first thing an OAuth2 client does with an error
 * response, which is {"error":"invalid_grant"} and no access_token, was a
 * segmentation fault.  Measured, not theorised.
 */
class missing_key : public exception {
public:
    missing_key(const std::string& msg = "") : exception(msg) {}
};

/** The input is not JSON, or is not only JSON. */
class parse_error : public exception {
public:
    parse_error(const std::string& msg = "") : exception("parse error: " + msg) {}
};

/**
 * One value, converted on demand.
 *
 * The conversions coerce across JSON types where the meaning survives -- an
 * int read as a double, the string "3599" read as an int -- and throw where it
 * does not.  They used to demand an exact json-c type, which is not what a
 * real endpoint sends: Microsoft's v1 token endpoint returns expires_in as the
 * *string* "3599", and a JSON number written 3599.0 is a double, so a client
 * insisting on json_type_int rejects both.
 */
class proxy {
public:
    /** The old spelling; jlib::util::json::type_mismatch is the same class. */
    typedef jlib::util::json::type_mismatch type_mismatch;

    /**
     * @param obj  the value, or null for absent
     * @param what what to call it in an error message -- a key, an index
     */
    proxy(json_object* obj, const std::string& what = "");

    /** Whether there is a value here at all.  Never throws. */
    bool present() const { return m_obj != 0; }

    operator std::string() const;
    operator std::size_t() const;
    operator int() const;
    operator unsigned int() const;
    operator int64_t() const;
    operator float() const;
    operator double() const;
    operator long double() const;
    operator bool() const;

    /** The value if it is there, and this if it is not.  Never throws. */
    std::string str_or(const std::string& dflt) const;
    int64_t int_or(int64_t dflt) const;
    double double_or(double dflt) const;
    bool bool_or(bool dflt) const;

protected:
    void require() const;

    json_object* m_obj;
    std::string m_what;
};

class object {
public:
    enum type { type_null, type_boolean, type_double, type_int, type_object, type_array, type_string };

    typedef std::shared_ptr<object> ptr;
    typedef std::shared_ptr<array> arrayptr;
    
    static ptr create();
    static ptr create(const std::string& data);
    
    virtual ~object();
    
    void add(const std::string& key, const std::string& val);
    void add(const std::string& key, int val);
    void add(const std::string& key, unsigned int val);
    void add(const std::string& key, double val);
    void add(const std::string& key, long double val);
    void add(const std::string& key, ptr val);
    void add(const std::string& key, arrayptr val);
    
    proxy get(const std::string& key) const;
    proxy get(std::size_t idx) const;

    /**
     * Whether the object has this key, whatever its value.
     *
     * json-c stores a JSON null as a null pointer, so get() alone cannot tell
     * an absent key from one whose value is null; this can, and it is what a
     * caller wanting to branch rather than catch should use.
     */
    bool has(const std::string& key) const;
    
    ptr obj(const std::string& key);
    ptr obj(unsigned int x);

    bool is(type t) const;
    
    std::string str(bool pretty = false) const;
    
    json_object* obj();
    
    std::size_t size() const;
    
    friend class array;
    
private:
    object();
    object(json_object* obj, bool put);
    object(const std::string& data);
    object(const object&);
    
    json_object* m_obj;
    bool m_put;
};
    
class array {
public:
    typedef std::shared_ptr<array> ptr;
    
    static ptr create();
    static ptr create(const std::string& data);
    
    virtual ~array();
    
    void add(const std::string& val);
    void add(int val);
    void add(unsigned int val);
    void add(double val);
    void add(long double val);
    void add(object::ptr val);
    void add(array::ptr val);
    
    proxy get(unsigned int x) const;
    object::ptr obj(unsigned int x) const;
    ptr arr(unsigned int x) const;
    
    std::string str() const;
    
    int size() const;
    
    json_object* obj();
    
    friend class object;
    
private:
    array();
    array(const std::string& data);
    array(json_object* obj);
    
    json_object* m_obj;
    bool m_put;
};

}
}
}

#endif

