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

    /**
     * The token written to the response pipe.  Its *value* means nothing --
     * handle() reads it and throws it away.  What matters is that a byte
     * arrives, because the response pipe's read end is what an event loop
     * selects on; see get_response_reader().
     *
     * There is no NEW_REQUEST any more, and no EXIT.  The request side used to
     * be a second pipe carrying those two ids, and it is a condition variable
     * now: nothing outside this class can see a request pipe, so nothing
     * outside needed a name for what went down it.
     */
    static const id_type NEW_RESPONSE = 0x1;
    
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
     * A read descriptor for the response pipe, for an event loop to watch.
     *
     * This is why the response half is a pipe and the request half is not.  A
     * GUI cannot block in wait(); it blocks in its own loop, on descriptors.
     * So a response has to arrive as a readable fd -- select, poll, epoll,
     * kqueue, g_io_add_watch, CFFileDescriptor, whichever the toolkit is --
     * and then the loop calls handle() to drain what is behind it.
     *
     * Requests travel the other way, from that loop into the worker, and the
     * worker *can* block.  Hence a condition variable there and a pipe here:
     * two different problems that only looked like one.
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
    
    pipe m_response_pipe;
    std::thread m_worker;
    std::mutex m_lock;

    // Guarded by m_requests' mutex, not atomic: it is read in the same
    // predicate as the queue's emptiness, and two independent atomics give no
    // guarantee that a worker seeing one sees the other.  This replaces both
    // the EXIT byte and the sync<bool> that backed it up when the byte could
    // not be written -- stop() cannot fail now, so it needs no fallback.
    bool m_exit = false;
    sys::sync<std::priority_queue<Request> > m_requests;
    sys::sync<std::queue<Response> > m_responses;
};
    
template<typename Request, typename Response>
inline
ASServent<Request,Response>::ASServent()
    : m_response_pipe(false,false)
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

    std::unique_lock<std::mutex> lock(m_requests);

    m_requests().push(r);

    // Cannot fail, which is the point.  This used to write a byte down a
    // non-blocking pipe and *swallow* would_block on a full one -- so a
    // request could be queued with nothing to announce it, which is precisely
    // why start() could not afford to block and polled at 1ms instead.
    //
    // notify_all rather than notify_one: there is only ever one worker, so the
    // two are the same call today, and this is the one that stays correct if
    // that ever stops being true.  Under the lock, per sync.hh's convention.
    m_requests.notify_all();
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
    // them on the same queue until the first noticed it was meant to go, and
    // which one served a given request was a race.
    stop();

    {
        std::unique_lock<std::mutex> lock(m_requests);

        m_exit = false;
    }

    m_worker = std::thread([this](){ this->start(); });
}

template<typename Request, typename Response>
inline
void 
ASServent<Request,Response>::stop() {
    if(!m_worker.joinable())
        return;

    {
        std::unique_lock<std::mutex> lock(m_requests);

        m_exit = true;

        m_requests.notify_all();
    }

    m_worker.join();
}
    
template<typename Request, typename Response>
inline
void 
ASServent<Request,Response>::start() {
    // Blocks.  It used to poll the request pipe with a one millisecond timeout
    // and then re-check the queue, so an idle worker woke about a thousand
    // times a second for the lifetime of the process -- measured at ~1.3% of a
    // core doing nothing at all, which for a mail client sitting in a tray is
    // a battery cost rather than a CPU one.
    while(true) {
        std::unique_lock<std::mutex> lock(m_requests);

        m_requests.wait(lock, [this] {
            return m_exit || !m_requests().empty();
        });

        // Checked before the queue, so stop() ends the loop promptly rather
        // than draining first.  A stop comes from a destructor or from
        // reset(); in the first, running more handlers is exactly what the
        // caller is trying to avoid, and in the second the requests are stale
        // by definition.  ASMailBox::reinit() already clear()s the queue
        // before reset()ing, which is the same view.
        if(m_exit)
            return;

        // Copy-initialised from top() rather than declared and assigned:
        // Request need not be default-constructible, and MailBoxRequest is
        // not.
        Request r = m_requests().top();

        m_requests().pop();

        // A handler can take as long as it likes, and can push() from inside
        // itself without deadlocking.
        lock.unlock();

        try {
            handle(r);
        }
        catch(std::exception& e) {
            std::cerr << "jlib::sys::ASServent::start(): got std::exception: " << e.what() << std::endl;
        }
        catch(...) {
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
