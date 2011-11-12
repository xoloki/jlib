/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joe Yandle <joey@divisionbyzero.com>
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

#ifndef JLIB_MATH_BUFFER_HH
#define JLIB_MATH_BUFFER_HH


#include <jlib/sys/object.hh>
#include <glibmm/refptr.h>

#include <cstring>


namespace jlib {
namespace math {


template<typename T>
class array : public sys::Object {
public:
    typedef Glib::RefPtr< array<T> > ptr;

    array(unsigned int size);
    virtual ~array();

    unsigned int size() const;

    T* data();
    const T* data() const;

    T& operator[](unsigned int i);
    const T& operator[](unsigned int i) const;

private:
    array(const array<T>& a);

    T* mdata;
    unsigned int msize;
};


template<typename T>
class buffer {
public:
    buffer();
    explicit buffer(unsigned int s);
    buffer(buffer<T> b, unsigned int o, unsigned int s);
    
    unsigned int size() const;

    T sum() const;
    T product() const;

    void resize(unsigned int s);

    T* data();
    const T* data() const;

    T& operator[](unsigned int i);
    const T& operator[](unsigned int i) const;

private:
    typename array<T>::ptr mbuf;
    unsigned int moff;
    unsigned int msize;
};


template<typename T>
inline
array<T>::array(unsigned int size) 
    : mdata(0),
      msize(size)
{
    mdata = new T[msize];
    std::memset(mdata, 0, size * sizeof(T));
}
    
template<typename T>
inline
array<T>::~array() {
    delete [] mdata;
}

template<typename T>
inline
unsigned int array<T>::size() const {
    return msize;
}

template<typename T>
inline
T* array<T>::data() {
    return mdata;
}

template<typename T>
inline
const T* array<T>::data() const {
    return mdata;
}

template<typename T>
inline
T& array<T>::operator[](unsigned int i) {
    return mdata[i];
}

template<typename T>
inline
const T& array<T>::operator[](unsigned int i) const {
    return mdata[i];
}

template<typename T>
inline
buffer<T>::buffer() 
    : mbuf(),
      moff(0),
      msize(0)
{
}

template<typename T>
inline
buffer<T>::buffer(unsigned int s) 
    : mbuf(new array<T>(s)),
      moff(0),
      msize(s)
{
}

template<typename T>
inline
buffer<T>::buffer(buffer<T> b, unsigned int o, unsigned int s)
    : mbuf(b.mbuf),
      moff(o),
      msize(s)
{
}

template<typename T>
inline
unsigned int buffer<T>::size() const {
    return msize;
}

template<typename T>
inline
T buffer<T>::sum() const {
    T s = 0;
    for(unsigned int i = 0; i < size(); i++) {
        s += *this[i];
    }
    return s;
}

template<typename T>
inline
T buffer<T>::product() const {
    T p = 1;
    for(unsigned int i = 0; i < size(); i++) {
        p *= *this[i];
    }
    return p;
}


template<typename T>
inline
void buffer<T>::resize(unsigned int s) {
    if(!mbuf || moff || size() < s) {
        mbuf = typename array<T>::ptr(new array<T>(s));
        moff = 0;
    } 
    msize = s;
}

template<typename T>
inline
T* buffer<T>::data() {
    return mbuf->data() + moff;
}


template<typename T>
inline
const T* buffer<T>::data() const {
    return mbuf->data() + moff;
}


template<typename T>
inline
T& buffer<T>::operator[](unsigned int i) {
    return mbuf->operator[](i + moff);
}

template<typename T>
inline
const T& buffer<T>::operator[](unsigned int i) const {
    return mbuf->operator[](i + moff);
}

    

}
}

#endif //JLIB_MATH_BUFFER_HH
