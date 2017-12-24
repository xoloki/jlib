/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2000 Joe Yandle <jwy@divisionbyzero.com>
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

#ifndef JLIB_SYS_SYNC_HH
#define JLIB_SYS_SYNC_HH

#include <thread>
#include <mutex>
#include <fstream>
#include <exception>
#include <string>
#include <functional>

#include <jlib/sys/auto.hh>

namespace jlib {
namespace sys {

class sync_exception : public std::exception {
public:
    sync_exception(std::string msg = "") {
        m_msg = "sys exception: "+msg;
    }
    virtual ~sync_exception() throw() {}
    virtual const char* what() const throw() { return m_msg.c_str(); }
protected:
    std::string m_msg;
};
    
template<class T>
void safe_set(T& t, const T& newval, std::mutex& m) {
    auto_lock<std::mutex> lock(m);
    
    t = newval;
}
    
template<class T>
T safe_get(const T& t, std::mutex& m) {
    T ret;
    auto_lock<std::mutex> lock(m);
    
    ret = t;
    
    return ret;
}
    
template<class T>
class sync {
public:
    typedef T                 value_type;
    typedef value_type&       reference;
    typedef const value_type& const_reference;
    typedef value_type*       pointer;
    typedef value_type* const const_pointer;
    
    sync() {}
    sync(const_reference val) { set(val); }
    
    reference operator()() { return ref(); }
    const_reference operator()() const { return ref(); }

    void operator()(const_reference val) { set(val); }

    operator T() const { return get(); }
    operator std::mutex&() { return m_lock; }

    sync<T>& operator=(const_reference val) { set(val); return *this; }

    value_type get() const {
        return safe_get(m_val, m_lock);
    }

    void set(const_reference val) {
        safe_set(m_val, val, m_lock);
    }

    reference ref() { return m_val; }
    const_reference ref() const { return m_val; }

    pointer operator->() { return &m_val; }
    const_pointer operator->() const { return &m_val; }

    std::mutex& mutex();

    void lock() { m_lock.lock(); }
    void unlock() { m_lock.unlock(); }

protected:
    mutable std::mutex m_lock;
    mutable T m_val;
private:
    sync(const sync<T>& copy);
};

}
}
#endif //JLIB_SYS_SYNC_HH
