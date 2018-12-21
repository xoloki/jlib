/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2010 Joe Yandle <xoloki@gmail.com>
 * 
 */

#include <json.h>

#include "json.hh"

#include <stdexcept>

namespace jlib {
namespace util {
namespace json {

proxy::proxy(json_object* obj) 
    : m_obj(obj)
{}
    
proxy::operator std::string() {
    // always okay to go to string
    //if(!json_object_is_type(m_obj, json_type_string))
	//throw type_mismatch();
    
    return json_object_get_string(m_obj);
}
    
proxy::operator std::size_t() {
    if(!json_object_is_type(m_obj, json_type_int))
	throw type_mismatch();
    
    return json_object_get_int(m_obj);
}
    
proxy::operator int() {
    if(!json_object_is_type(m_obj, json_type_int))
	throw type_mismatch();
    
    return json_object_get_int(m_obj);
}
    
proxy::operator int64_t() {
    if(!json_object_is_type(m_obj, json_type_int))
	throw type_mismatch();
    
    return json_object_get_int64(m_obj);
}
    
proxy::operator bool() {
    return (m_obj != 0);
}
    
proxy::operator unsigned int() {
    if(!json_object_is_type(m_obj, json_type_int))
	throw type_mismatch();
    
    return json_object_get_int(m_obj);
}
    
proxy::operator double() {
    if(!json_object_is_type(m_obj, json_type_double))
	throw type_mismatch();
    
    return json_object_get_double(m_obj);
}
    
proxy::operator float() {
    if(!json_object_is_type(m_obj, json_type_double))
	throw type_mismatch();
    
    return static_cast<float>(json_object_get_double(m_obj));
}
    
proxy::operator long double() {
    if(!json_object_is_type(m_obj, json_type_double))
	throw type_mismatch();
    
    return json_object_get_double(m_obj);
}
    
object::ptr object::create() { 
    return ptr(new object());
}
    
object::ptr object::create(std::string data) { 
    return ptr(new object(data));
}
    
object::object() 
    : m_obj(json_object_new_object()),
      m_put(true)
{}
    
object::object(std::string data) 
    : m_obj(json_tokener_parse(data.data())),
      m_put(true)
{
    if(is_error(m_obj))
	throw std::runtime_error("data did not parse to JSON: '" + data + "'");
}
    
object::object(json_object* obj, bool put) 
    : m_obj(obj),
      m_put(put)
{
}
    
object::~object() { 
    if(m_put)
	json_object_put(m_obj); 
}
    
proxy object::get(std::string key) const {
    return proxy(json_object_object_get(m_obj, key.data()));
}
    
proxy object::get(std::size_t idx) const {
    return proxy(json_object_array_get_idx(m_obj, idx));
}
    
void object::add(std::string key, std::string val) {
    json_object_object_add(m_obj, key.data(), json_object_new_string(const_cast<char*>(val.data())));
}
    
void object::add(std::string key, int val) {
    json_object_object_add(m_obj, key.data(), json_object_new_int(val));
}
    
void object::add(std::string key, unsigned int val) {
    add(key, static_cast<int>(val));
}
    
void object::add(std::string key, double val) {
    json_object_object_add(m_obj, key.data(), json_object_new_double(val));
}
    
void object::add(std::string key, long double val) {
    add(key, static_cast<double>(val));
}
    
void object::add(std::string key, object::ptr val) {
    json_object_object_add(m_obj, key.data(), val->obj());
    val->m_put = false;
}
    
void object::add(std::string key, object::arrayptr val) {
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
    
object::ptr object::obj(std::string key) {
    json_object* o = json_object_object_get(m_obj, key.data());
    return ptr(new object(o, false));
}
    
object::ptr object::obj(unsigned int x) {
    json_object* o = json_object_array_get_idx(m_obj, x);
    return ptr(new object(o, false));
}
    
std::size_t object::size() const {
    return json_object_array_length(m_obj);
}
    
bool object::is(object::type t) const {
    return json_object_is_type(m_obj, (json_type)(int)t);
}
    
array::ptr array::create() { 
    return ptr(new array());
}
    
array::ptr array::create(std::string data) { 
    return ptr(new array(data));
}
    
array::array() 
    : m_obj(json_object_new_array()),
      m_put(true)
{}
    
array::array(std::string data) 
    : m_obj(json_tokener_parse(data.data())),
      m_put(true)
{
    if(is_error(m_obj))
	throw std::runtime_error("data did not parse to JSON: '" + data + "'");
}
    
array::array(json_object* obj) 
    : m_obj(obj),
      m_put(false)
{
}
    
array::~array() { 
    if(m_put)
        json_object_put(m_obj); 
}
    
void array::add(std::string val) {
    json_object_array_add(m_obj, json_object_new_string(const_cast<char*>(val.data())));
}
    
void array::add(int val) {
    json_object_array_add(m_obj, json_object_new_int(val));
}
    
void array::add(unsigned int val) {
    add(static_cast<int>(val));
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
    return proxy(json_object_array_get_idx(m_obj, x));
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
