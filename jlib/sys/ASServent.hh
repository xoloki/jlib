/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2002 Joey Yandle <xoloki@gmail.com>
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

#ifndef JLIB_SYS_ASSERVENT_HH
#define JLIB_SYS_ASSERVENT_HH

#include <jlib/sys/pipe.hh>
#include <jlib/sys/sync.hh>
#include <jlib/sys/auto.hh>

#include <exception>
#include <iostream>
#include <string>
#include <sstream>
#include <map>
#include <mutex>
#include <thread>
#include <queue>

#include <cstring>

#include <errno.h>

namespace jlib {
namespace sys {

/**
 * ASServent is an asynchronous Servent class that is templated on the request and 
 * reponse types.
 *
 * the Request and Response types should be trivially copyable
 * Request should override operator<() based on priority
 */
template<typename Request, typename Response>
class ASServent {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = "jlib::sys::ASServent<Request,Response> exception"+
                (msg != "" ? (": "+msg):"");
        }
        virtual ~exception() {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }
        
        static void throw_errno(const std::string& msg) {
            std::ostringstream o;
            o << ((msg!="")?(msg+": "):"") << strerror(errno);
            throw exception(o.str());
        }
        
    protected:
        std::string m_msg;
    };
    
    typedef int id_type;
    static const id_type NEW_REQUEST =  0x0;
    static const id_type NEW_RESPONSE = 0x1;
    
    static const id_type EXIT =         0x666;
    
    ASServent();
    virtual ~ASServent();
    
    void push(const Request& r);
    
    void push(const Response& r);
    
    void run();
    
    void reset();
    
    /**
     * Stop the worker and wait for it to finish.
     *
     * A derived class must call this from its own destructor.  ~ASServent
     * calls it too, but by then the derived part of the object is already
     * gone, so anything the worker touches that a subclass owns must be shut
     * down earlier than that -- and handle(Request) is pure virtual here, so
     * a worker still running when the derived destructor has finished is a
     * pure virtual call, not merely a data race.
     *
     * Idempotent, and safe on an ASServent that was never run().
     */
    void stop();
    
    void start();
    
    /**
     * get a read descriptor for the response pipe
     */
    int get_response_reader();
    
    /**
     * in the worker thread, execute the request r, and push() responses
     */
    virtual void handle(const Request& r) = 0;
    
    /**
     * in the main thread, handle the response (e.g. send signals)
     */
    virtual void handle(const Response& r) = 0;
    
    /**
     * connect the response reader to this method so it will call handle(Response)
     * appropriately
     */
    void handle();
    
protected:
    
    pipe m_request_pipe;
    pipe m_response_pipe;
    std::thread m_worker;
    std::mutex m_lock;

    // The loop's own flag, and stop()'s fallback for when the EXIT byte cannot
    // be written.  start() used to test a *local* bool, so the pipe was the
    // only way to end the loop -- and the request pipe is opened non-blocking,
    // so a full one makes write_int throw and there was no second way to ask.
    // That would turn the leak this fixes into a hang in join().  Named to
    // match Servent::m_bunny, which is the same flag for the same reason.
    sync<bool> m_bunny;
    sys::sync<std::priority_queue<Request> > m_requests;
    sys::sync<std::queue<Response> > m_responses;
};
    
template<typename Request, typename Response>
inline
ASServent<Request,Response>::ASServent()
    : m_request_pipe(false,false),
      m_response_pipe(false,false),
      m_bunny(true)
{
    
}
    
template<typename Request, typename Response>
inline
ASServent<Request,Response>::~ASServent() {
    // Backstop only.  By the time this runs the derived part of the object is
    // already destroyed, so subclasses owning anything the worker touches must
    // call stop() from their own destructor.  Without this the worker outlived
    // the object entirely, which is strictly worse than the narrow window that
    // remains.
    stop();
}
    
template<typename Request, typename Response>
inline
void 
ASServent<Request,Response>::push(const Request& r) {
    if(getenv("JLIB_SYS_ASSERVENT_DEBUG"))
        std::cerr << "jlib::sys::ASServent::push(Request): enter" << std::endl;
    auto_lock<std::mutex> lock(m_requests);
    m_requests().push(r);
    try {
        if(getenv("JLIB_SYS_ASSERVENT_DEBUG"))
            std::cerr << "jlib::sys::ASServent::push(Request): writing to pipe" << std::endl;
        m_request_pipe.write_int(NEW_REQUEST);
    } catch(sys::pipe::would_block&) {
        if(getenv("JLIB_SYS_ASSERVENT_DEBUG"))
            std::cerr << "jlib::sys::ASServent::push(Request): caught would_block" << std::endl;
    }
    
}
    
template<typename Request, typename Response>
inline
void 
ASServent<Request,Response>::run() {
    reset();
}
    
template<typename Request, typename Response>
inline
void 
ASServent<Request,Response>::reset() {
    // Joined, not merely asked to leave.  Dropping the old worker left two of
    // them reading the same request pipe until the first noticed EXIT, and
    // which one served a given request was a race.
    stop();

    m_bunny = true;

    m_worker = std::thread([this](){ this->start(); });
}

template<typename Request, typename Response>
inline
void 
ASServent<Request,Response>::stop() {
    if(!m_worker.joinable())
        return;

    try {
        m_request_pipe.write_int(EXIT);
    }
    catch(std::exception& e) {
        // The request pipe is non-blocking and may be full, or already
        // unwritable.  Ask the loop to leave directly rather than joining a
        // thread that was never told to.
        std::cerr << "jlib::sys::ASServent::stop(): " << e.what() << std::endl;
        m_bunny = false;
    }

    m_worker.join();
}
    
template<typename Request, typename Response>
inline
void 
ASServent<Request,Response>::start() {
    while(m_bunny) {
        try {
            while(m_request_pipe.poll()) {
                if(getenv("JLIB_SYS_ASSERVENT_DEBUG"))
                    std::cerr << "jlib::sys::ASServent::start(): m_request_pipe.poll(): true" << std::endl;
                id_type id = m_request_pipe.read_int();
                
                if(id == NEW_REQUEST) {
                    
                } else if(id == EXIT) {
                    m_bunny = false;
                    break;
                }
            }
            
            auto_lock<std::mutex> lock(m_requests);
            while(m_requests().size() > 0) {
                Request r = m_requests().top();
                m_requests().pop();
                
                m_requests.unlock();
                
                try { handle(r); } catch(...) {}
                
                m_requests.lock();
            }
            
        } catch(std::exception& e) {
            std::cerr << "jlib::sys::ASServent::start(): got std::exception: " << e.what() << std::endl;
        } catch(...) {
            std::cerr << "jlib::sys::ASServent::start(): got unknown error" << std::endl;
        }
    }
}
        
template<typename Request, typename Response>
inline
int 
ASServent<Request,Response>::get_response_reader() {
    return m_response_pipe.get_reader();
}
    
template<typename Request, typename Response>
inline
void 
ASServent<Request,Response>::push(const Response& r) {
    if(getenv("JLIB_SYS_ASSERVENT_DEBUG"))
        std::cerr << "jlib::sys::ASServent::push(Response): enter" << std::endl;
    
    auto_lock<std::mutex> lock(m_responses);
    m_responses().push(r);
    try {
        if(getenv("JLIB_SYS_ASSERVENT_DEBUG"))
            std::cerr << "jlib::sys::ASServent::push(Response): writing to pipe" << std::endl;
        m_response_pipe.write_int(NEW_RESPONSE);
    } catch(sys::pipe::would_block&) {
        if(getenv("JLIB_SYS_ASSERVENT_DEBUG"))
            std::cerr << "jlib::sys::ASServent::push(Response): caught pipe::would_block" << std::endl;
    }
}

template<typename Request, typename Response>
inline
void 
ASServent<Request,Response>::handle() {
    id_type id = m_response_pipe.read_int();

    auto_lock<std::mutex> lock(m_responses);
    while(m_responses().size() > 0) {
        Response r = m_responses().front();
        m_responses().pop();
        m_responses.unlock();

        try { this->handle(r); } catch(...) { m_responses.lock(); throw; }

        m_responses.lock();
    }
}
}
}

#endif //JLIB_SYS_ASSERVENT_HH
