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

#include <memory>

struct json_object;

namespace jlib {
namespace util {
namespace json {

class array;

class proxy {
public:
    class type_mismatch : public std::exception {};
    
    proxy(json_object* obj);
    
    operator std::string();
    operator std::size_t();
    operator int();
    operator unsigned int();
    operator int64_t();
    operator float();
    operator double();
    operator long double();
    operator bool();
    
protected:
    json_object* m_obj;
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

