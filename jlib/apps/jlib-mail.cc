/* -*- mode: C++ c-basic-offset: 4 -*-
 * 
 * Copyright (c) 1999 Joe Yandle <jwy@divisionbyzero.com>
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

#include <jlib/net/net.hh>
#include <jlib/net/Imap4Folder.hh>

#include <jlib/sys/sys.hh>
#include <jlib/sys/object.hh>

#include <jlib/util/util.hh>
#include <jlib/util/URL.hh>

#include <sigc++/sigc++.h>

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <exception>
#include <sstream>
#include <cstdlib>

#define CATCH_EXCEPTIONS 1

namespace jlib {
    namespace app {
        
        class ConsoleMain : public sys::Object {
        public:
            void push(std::string cmd, sigc::slot1<void,std::string> slot) {
                m_handlers.insert(std::make_pair(cmd,slot));
            }

            void event_loop() {
                std::string buf;
                while(true) {
                    std::cout << prompt() << std::flush;
                    std::getline(std::cin,buf);
                    std::map<std::string,sigc::slot1<void,std::string> >::iterator i;
                    for(i=m_handlers.begin();i!=m_handlers.end();i++) {
                        if(buf.find(i->first) == 0) {
                            std::string cmd = jlib::util::trim(buf.substr(i->first.length()));
#if CATCH_EXCEPTIONS
                            try {
#endif
                                i->second(cmd);
#if CATCH_EXCEPTIONS
                            }
                            catch(std::exception& e) {
                                std::cout << e.what() << std::endl;
                            }
#endif
                        }
                    }
                }
            }
            
            std::string prompt() {
                return (m_base+m_tag);
            }

        protected:
            std::map<std::string, sigc::slot1<void,std::string> > m_handlers;
            std::string m_base;
            std::string m_tag;
            
        };


        class MailClient : public ConsoleMain {
        public:
            MailClient(std::string url="") {
                init();
                if(url != "") {
                    open(url);
                }
            }
            
            void init() {
                m_base = "mail";
                m_tag = "> ";
                m_folder = 0;
 
                push("open", sigc::mem_fun(*this,&jlib::app::MailClient::open));
                push("scan", sigc::mem_fun(*this,&jlib::app::MailClient::scan));
                push("print", sigc::mem_fun(*this,&jlib::app::MailClient::print));
                push("read", sigc::mem_fun(*this,&jlib::app::MailClient::read));                
            }

            void scan(std::string) {
                if(m_folder == 0) {
                    std::cout << "ERROR: no valid mailfolder"<<std::endl;
                    return;
                }
                std::cout << "Scanning mailfolder... " << std::flush;
                m_folder->scan();
                std::cout << "found "<< m_folder->size()<<" messages"<<std::endl;
                
            }
            
            void print(std::string) {
                if(m_folder == 0) {
                    std::cout << "ERROR: no valid mailfolder"<<std::endl;
                    return;
                }
                jlib::net::MailFolder::iterator i = m_folder->begin();
                unsigned int j=0;
                for(;i!=m_folder->end();i++) {
                    jlib::util::Headers head(i->headers());
#if HAVE_BITS_CHAR_TRAITS_H
                    std::cout << std::setw(4) << std::right <<j<< 
                        std::left << ": ";
#else
                    std::cout << std::setw(4) <<j<< ": ";
#endif
                    printw(std::cout,head["DATE"],25);
                    std::cout << " || ";
                    printw(std::cout,head["FROM"],25);
                    std::cout << " || ";
                    printw(std::cout,head["SUBJECT"],55);
                    std::cout << std::endl;
                    j++;
                }
            }


            void open(std::string s) {
                jlib::util::URL url(s);
                if(m_folder != 0) {
                    delete m_folder;
                    m_folder = 0;
                }

                std::string prompt = "Enter password for " + s + "> ";
                char* pass = ::getpass(prompt.c_str());
                url.set_pass(pass);

                if(jlib::util::lower(url.get_protocol()).find("imap") != std::string::npos) {
                    m_folder = new jlib::net::Imap4Folder(url);
                    set_base(url);
                }
                //else 

            }
   
            void read(std::string s) {
                if(m_folder == 0) {
                    std::cout << "ERROR: no valid mailfolder"<<std::endl;
                    return;
                }
                unsigned int k = jlib::util::int_value(s);

                std::cout << "reading msg "<<k<<"... "<<std::flush;
                jlib::net::Email e(m_folder->get(k));
                std::cout << "done"<<std::endl;
                
                if(getenv("JLIB_MAIL_GLOB_TEXT"))
                    std::cout << e.get_globbed_text() << std::endl;
                else if(getenv("JLIB_MAIL_PRIMARY_TEXT"))
                    std::cout << e.get_primary_text() << std::endl;
                else
                    std::cout << e.data() << std::endl;
            }

         
        protected:
            std::ostream& printw(std::ostream& out, std::string in, unsigned int w) {
                if(in.length() >= w) {
                    in = in.substr(0,w);
                }
                //out << std::setw(w) << std::setfill(' ') << std::left <<in;
                out << in;
                return out;
            }

            void set_base(jlib::util::URL url) {
                jlib::util::URL tmp;
                tmp.set_protocol(url.get_protocol());
                tmp.set_user(url.get_user());
                tmp.set_host(url.get_host());
                tmp.set_path(url.get_path());
                m_base = tmp();                
            }

            jlib::net::MailFolder* m_folder;
            
        };

    }
}

int main(int argc, char** argv) {
#if CATCH_EXCEPTIONS
    try {
#endif
        std::string url;

        if(argc > 1) {
            url = argv[1];
        }

	//Glib::thread_init();
        jlib::app::MailClient client(url);
        client.event_loop();
#if CATCH_EXCEPTIONS
    }
    catch(std::exception& e) {
        std::cerr << "caught exception: "<<e.what() << std::endl;
    }
#endif
}
