/* -*- mode: C++ c-basic-offset: 4  -*-
 * 
 * Copyright (c) 2008 Joe Yandle <joey@divisionbyzero.com>
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

#ifndef JLIB_SYS_JOYSTICK_HH
#define JLIB_SYS_JOYSTICK_HH

#include <glibmm/thread.h>

#include <sigc++/slot.h>

#include <iostream>
#include <sstream>
#include <exception>
#include <string>
#include <functional>

#include <cstring>

#include <linux/joystick.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_AXES 16

namespace jlib {
    namespace sys {

        class joystick {
        public:
            class exception : public std::exception {
            public:
                exception(std::string msg = "") {
                    m_msg = "sys::joystick exception: " + msg;
                }
                exception(std::string msg, int e) {
                    m_msg = "sys::joystick exception: " + msg + std::strerror(e);
                }
                virtual ~exception() throw() {}
                virtual const char* what() const throw() { return m_msg.c_str(); }
            protected:
                std::string m_msg;
            };

            struct corrections {
                corrections(joystick& j) : joy(j) {}

                joystick& joy;
                js_corr jscs[MAX_AXES];
            };

            struct event {
                js_event jse;
            };

            joystick(std::string device, bool open = true, bool write = false) 
                : m_device(device),
                  m_fd(-1)
            {
                this->open(write);
            }
            
            ~joystick() {
                close();
            }

            void open(bool write = false) {
                std::cerr << "sys::joystick::open: " << m_device << std::endl;
                m_fd = ::open(m_device.c_str(), write ? O_RDWR : O_RDONLY);
                if(m_fd == -1) 
                    throw exception("opening device", errno);

                
            }

            void close() {
                std::cerr << "sys::joystick::close: " << m_device << std::endl;
                if(m_fd != -1) {
                    ::close(m_fd);
                }
            }

            event read() {
                event e;
                int err = ::read(m_fd, &e.jse, sizeof(struct js_event));
                if(err == -1) 
                    throw exception("reading event", errno);

                return e;
            }

            std::string get_name() {
                std::string name(128, 0);
                if (ioctl(m_fd, JSIOCGNAME(name.size()), name.data()) < 0)
                    throw exception("reading name", errno);
                
                return name;
            }
            
            int get_n_buttons() {
                char n;
                if(ioctl(m_fd, JSIOCGBUTTONS, &n) == -1) 
                    throw exception("reading number of buttons", errno);
                
                return n;
            }
            
            int get_n_axes() {
                char n;
                if(ioctl(m_fd, JSIOCGAXES, &n) == -1) 
                    throw exception("reading number of buttons", errno);
                
                return n;
            }

            std::string get_axes_map() {
                std::string data(ABS_MAX + 1, 0);
                if (ioctl(m_fd, JSIOCGAXMAP, data.data()) < 0)
                    throw exception("reading axes map", errno);
                
                return data;
            }
            
            std::string set_axes_map(std::string amap) {
                std::string data(ABS_MAX + 1, 0);

                if (ioctl(m_fd, JSIOCSAXMAP, amap.data()) < 0)
                    throw exception("writing axes map", errno);
                

                if (ioctl(m_fd, JSIOCGAXMAP, data.data()) < 0)
                    throw exception("reading axes map", errno);
                
                return data;
            }

            std::string get_button_map() {
                const int N = 2 * (KEY_MAX - BTN_MISC + 1);
                std::string data(N, 0);
                if (ioctl(m_fd, JSIOCGBTNMAP, data.data()) < 0)
                    throw exception("reading button map", errno);
                
                return data;
            }
            
            std::string set_button_map(std::string amap) {
                std::string data(KEY_MAX - BTN_MISC + 1, 0);

                if (ioctl(m_fd, JSIOCSBTNMAP, amap.data()) < 0)
                    throw exception("writing button map", errno);
                

                if (ioctl(m_fd, JSIOCGBTNMAP, data.data()) < 0)
                    throw exception("reading button map", errno);
                
                return data;
            }

            corrections get_corrections() {
                corrections c(*this);

                if (ioctl(m_fd, JSIOCGCORR, &c.jscs) < 0)
                    throw exception("reading corrections", errno);
                
                return c;
            }
            
            corrections set_correction(corrections c) {
                if (ioctl(m_fd, JSIOCSCORR, &c.jscs) < 0)
                    throw exception("writing corrections", errno);
                

                if (ioctl(m_fd, JSIOCGCORR, &c.jscs) < 0)
                    throw exception("reading corrections", errno);
                
                return c;
            }
            
        protected:
            std::string m_device;
            int m_fd;
        };
        
    }
}

std::ostream& operator<<(std::ostream& o, const jlib::sys::joystick::corrections& c) {
    o << "[";
    for(int i = 0; i < c.joy.get_n_axes(); i++) {
        o << "[";
        switch(c.jscs[i].type) {
        case JS_CORR_NONE:
            o << "none";
            break;
        case JS_CORR_BROKEN:
            o << "none";
            break;
        }
        o << "]";
    }
    o << "]";

    return o;
}


std::ostream& operator<<(std::ostream& o, const jlib::sys::joystick::event& e) {
    if(e.jse.type & JS_EVENT_INIT) 
        o << "initialize ";

    if(e.jse.type & JS_EVENT_BUTTON) {
        o << "button " << static_cast<int>(e.jse.number) << ": " << e.jse.value;
    } else if(e.jse.type & JS_EVENT_AXIS) {
        o << "axis " << static_cast<int>(e.jse.number) << ": " << e.jse.value;
    } else {
        o << "unknown-type [" << static_cast<int>(e.jse.type) << "]";;
    }

    return o;
}


#endif //JLIB_SYS_SYNC_HH
