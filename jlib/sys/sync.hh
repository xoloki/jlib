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

#include <glibmm/thread.h>

#include <sigc++/slot.h>

#include <jlib/sys/auto.hh>

#include <fstream>
#include <exception>
#include <string>
#include <functional>

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
        void safe_set(T& t, const T& newval, Glib::Mutex& m) {
            auto_lock<Glib::Mutex> lock(m);

            t = newval;
        }

        template<class T>
        T safe_get(const T& t, Glib::Mutex& m) {
            T ret;
            auto_lock<Glib::Mutex> lock(m);

            ret = t;

            return ret;
        }
        
        template<class Ret, class Func, class T>
        Ret safe_call(Func f, T& x, Glib::Mutex& m) {
            Ret ret;
            auto_lock<Glib::Mutex> lock(m);

            ret = f(x);

            return ret;
        }

        template<class Func, class T>
        void safe_call(Func f, T& x, Glib::Mutex& m) {
            auto_lock<Glib::Mutex> lock(m);

            f(x);
        }

	class GlibThreadInit {
	public:
	    GlibThreadInit() { 
		static bool already = false;
		if (!already) {
		    already = true;
		    Glib::thread_init();
		}
	    }
	};

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
            operator Glib::Mutex&() { return m_lock; }

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

            template<class F, class rettype, class Arg>
            rettype call(rettype (F::*f)(Arg), const Arg& a) {
                return safe_call<rettype>(std::bind2nd(std::mem_fun_ref(f),a), m_val, m_lock);
            }
            
            template<class F, class rettype, class Arg>
            rettype call(rettype (F::*f)(Arg) const, const Arg& a) const {
                return safe_call<rettype>(std::bind2nd(std::mem_fun_ref(f),a), m_val, m_lock);
            }
            
            template<class F, class Arg>
            void call(void (F::*f)(Arg), const Arg& a) {
                safe_call(std::bind2nd(std::mem_fun_ref(f),a), m_val, m_lock);
            }
            
            template<class F, class Arg>
            void call(void (F::*f)(Arg) const, const Arg& a) const {
                safe_call(std::bind2nd(std::mem_fun_ref(f),a), m_val, m_lock);
            }
            
            template<class F, class rettype>
            rettype call(rettype (F::*f)(void)) {
                return safe_call<rettype>(std::mem_fun_ref(f), m_val, m_lock);
            }
            
            template<class F, class rettype>
            rettype call(rettype (F::*f)(void) const) const {
                //return safe_call< const_mem_fun_ref_t<rettype,F>, T, rettype >(std::mem_fun_ref(f), m_val, m_lock);
                return safe_call<rettype>(std::mem_fun_ref(f), m_val, m_lock);
            }
            
            template<class F>
            void call(void (F::*f)(void)) {
                safe_call(std::mem_fun(f), m_val, m_lock);
            }

            template<class F>
            void call(void (F::*f)(void) const) const {
                safe_call(std::mem_fun(f), m_val, m_lock);
            }

            Glib::Mutex& mutex();

            void lock() { m_lock.lock(); }
            void unlock() { m_lock.unlock(); }
            bool trylock() { return m_lock.trylock(); }

        protected:
	    GlibThreadInit m_init;
            mutable Glib::Mutex m_lock;
            mutable T m_val;
	private:
	    sync(const sync<T>& copy);
        };

    }
}
#endif //JLIB_SYS_SYNC_HH
