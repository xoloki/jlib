/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2010 Joe Yandle <xoloki@gmail.com>
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
    static ptr create(std::string data);
    
    virtual ~object();
    
    void add(std::string key, std::string val);
    void add(std::string key, int val);
    void add(std::string key, unsigned int val);
    void add(std::string key, double val);
    void add(std::string key, long double val);
    void add(std::string key, ptr val);
    void add(std::string key, arrayptr val);
    
    proxy get(std::string key) const;
    proxy get(std::size_t idx) const;
    
    ptr obj(std::string key);
    ptr obj(unsigned int x);

    bool is(type t) const;
    
    std::string str(bool pretty = false) const;
    
    json_object* obj();
    
    std::size_t size() const;
    
    friend class array;
    
private:
    object();
    object(json_object* obj, bool put);
    object(std::string data);
    object(const object&);
    
    json_object* m_obj;
    bool m_put;
};
    
class array {
public:
    typedef std::shared_ptr<array> ptr;
    
    static ptr create();
    static ptr create(std::string data);
    
    virtual ~array();
    
    void add(std::string val);
    void add(int val);
    void add(unsigned int val);
    void add(double val);
    void add(long double val);
    void add(object::ptr val);
    void add(array::ptr val);
    
    proxy get(unsigned int x) const;
    
    std::string str() const;
    
    int size() const;
    
    json_object* obj();
    
    friend class object;
    
private:
    array();
    array(std::string data);
    array(const object&);
    
    json_object* m_obj;
    bool m_put;
};

}
}
}

#endif

