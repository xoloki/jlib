/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2002 Joe Yandle <jwy@divisionbyzero.com>
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

#ifndef JLIB_SYS_ASSERVENT_HH
#define JLIB_SYS_ASSERVENT_HH

#include <glibmm/thread.h>
#include <glibmm/object.h>
#include <jlib/sys/pipe.hh>
#include <jlib/sys/sync.hh>
#include <jlib/sys/auto.hh>

#include <exception>
#include <string>
#include <sstream>
#include <map>
#include <queue>

#include <cstring>

#include <sigc++/sigc++.h>

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
        class ASServent : public Glib::Object {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = "jlib::sys::ASServent<Request,Response> exception"+
                        (msg != "" ? (": "+msg):"");
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
                
                static void throw_errno(std::string msg) {
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
            Glib::Thread* m_worker;
            Glib::Mutex m_lock;
            sys::sync<std::priority_queue<Request> > m_requests;
            sys::sync<std::queue<Response> > m_responses;
        };

        template<typename Request, typename Response>
        inline
        ASServent<Request,Response>::ASServent()
            : m_worker(0),
              m_request_pipe(false,false),
              m_response_pipe(false,false)
        {
            
        }

        template<typename Request, typename Response>
        inline
        ASServent<Request,Response>::~ASServent() {
            
        }

        template<typename Request, typename Response>
        inline
        void 
        ASServent<Request,Response>::push(const Request& r) {
            if(getenv("JLIB_SYS_ASSERVENT_DEBUG"))
                std::cerr << "jlib::sys::ASServent::push(Request): enter" << std::endl;
            auto_lock<Glib::Mutex> lock(m_requests);
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
            if(m_worker) {
                m_request_pipe.write_int(EXIT);
                m_worker = 0;
            }
            m_worker = Glib::Thread::create(sigc::mem_fun(this, &jlib::sys::ASServent<Request,Response>::start), false);
        }
        
        template<typename Request, typename Response>
        inline
        void 
        ASServent<Request,Response>::start() {
            while(true) {
                try {
                    while(m_request_pipe.poll()) {
                        if(getenv("JLIB_SYS_ASSERVENT_DEBUG"))
                            std::cerr << "jlib::sys::ASServent::start(): m_request_pipe.poll(): true" 
                                      << std::endl;
                        id_type id = m_request_pipe.read_int();
                                     
                        if(id == NEW_REQUEST) {
                            
                        } else if(id == EXIT) {
                            throw Glib::Thread::Exit();
                        }
                    }

                    auto_lock<Glib::Mutex> lock(m_requests);
                    while(m_requests().size() > 0) {
                        Request r = m_requests().top();
                        m_requests().pop();

                        m_requests.unlock();

                        try { handle(r); } catch(...) {}

                        m_requests.lock();
                    }
                    
                } catch(Glib::Thread::Exit& e) {
                    throw e;
                } catch(std::exception& e) {
                } catch(...) {
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
                          
            auto_lock<Glib::Mutex> lock(m_responses);
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

            auto_lock<Glib::Mutex> lock(m_responses);
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
