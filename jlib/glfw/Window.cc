/* -*- mode: C++ c-basic-offset: 4 -*-
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

#include <jlib/glfw/Window.hh>

#include <GLFW/glfw3.h>

#include <chrono>
#include <cctype>
#include <iostream>
#include <sstream>
#include <thread>

namespace jlib {
namespace glfw {

namespace {

/**
 * glfwInit/glfwTerminate are global and have to be paired.  Tying them to a
 * function-local static initializes once, on first use, and tears down at
 * exit -- rather than per window, which would terminate the library under
 * any other window still open.
 */
struct Library {
    Library() {
        glfwSetErrorCallback([](int code, const char* text) {
            std::cerr << "glfw error " << code << ": "
                      << (text ? text : "(no description)") << std::endl;
        });

        if(!glfwInit())
            throw Window::exception("glfwInit failed");
    }

    ~Library() {
        glfwTerminate();
    }
};

void require_glfw() {
    static Library library;
}

}

Window::Window(std::string title, int width, int height, bool depth)
    : m_window(0),
      m_title(title),
      m_timeout(10000),
      m_configured(false)
{
    require_glfw();

    // No GLFW_OPENGL_PROFILE and no version hint: that yields a legacy
    // context, which is what fixed-function rendering needs.  See the note in
    // Window.hh before changing this.
    glfwWindowHint(GLFW_DEPTH_BITS, depth ? 24 : 0);
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);

    m_window = glfwCreateWindow(width, height, title.c_str(), 0, 0);
    if(m_window == 0)
        throw exception("glfwCreateWindow failed");

    glfwSetWindowUserPointer(m_window, this);

    glfwSetKeyCallback(m_window, &Window::on_key);
    glfwSetMouseButtonCallback(m_window, &Window::on_button);
    glfwSetFramebufferSizeCallback(m_window, &Window::on_framebuffer_size);

    make_current();

    // Without this the swap interval is driver-dependent; pinning it to the
    // refresh keeps the plots from spinning as fast as the CPU allows.
    glfwSwapInterval(1);
}

Window::~Window() {
    if(m_window != 0) {
        glfwDestroyWindow(m_window);
        m_window = 0;
    }
}

GLFWwindow* Window::get_handle() {
    return m_window;
}

void Window::make_current() {
    glfwMakeContextCurrent(m_window);
}

int Window::get_width() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    return w;
}

int Window::get_height() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    return h;
}

void Window::set_title(std::string title) {
    m_title = title;
    glfwSetWindowTitle(m_window, title.c_str());
}

void Window::center() {
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if(monitor == 0)
        return;

    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    if(mode == 0)
        return;

    int mx = 0, my = 0;
    glfwGetMonitorPos(monitor, &mx, &my);

    int w = 0, h = 0;
    glfwGetWindowSize(m_window, &w, &h);

    glfwSetWindowPos(m_window, mx + (mode->width - w) / 2, my + (mode->height - h) / 2);
}

void Window::clear() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Window::flush() {
    glfwSwapBuffers(m_window);
}

bool Window::should_close() const {
    return glfwWindowShouldClose(m_window) != 0;
}

void Window::set_should_close(bool close) {
    glfwSetWindowShouldClose(m_window, close ? GLFW_TRUE : GLFW_FALSE);
}

void Window::set_timeout(long micro) {
    m_timeout = micro;
}

long Window::get_timeout() const {
    return m_timeout;
}

void Window::iterate() {
    glfwPollEvents();

    // GLFW delivers no resize event for the initial size, so synthesize one
    // before the first frame.
    //
    // Deliberately here rather than in a constructor.  A subclass's override
    // of the handler is not reachable until its own construction has
    // finished, so configuring from a base constructor silently runs the base
    // version -- which is what left jhardhyper blank until it was resized: it
    // got the base's pixel-space ortho2d instead of its own perspective, and
    // its unit-scale geometry landed in a corner pixel.
    if(!m_configured) {
        m_configured = true;
        configure_notify.emit(get_width(), get_height());
    }

    if(m_timeout > 0)
        std::this_thread::sleep_for(std::chrono::microseconds(m_timeout));

    timeout.emit();
}

void Window::run() {
    while(!should_close()) {
        iterate();
    }
}

std::string Window::key_text(int key, int scancode, int mods) {
    // Keys that produce a control character: XLookupString hands these back
    // as text, and Hyper.hh's handlers compare against ' '.
    switch(key) {
    case GLFW_KEY_SPACE:     return " ";
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:  return "\r";
    case GLFW_KEY_TAB:       return "\t";
    case GLFW_KEY_BACKSPACE: return "\b";
    case GLFW_KEY_ESCAPE:    return "\x1b";
    default:                 break;
    }

    // Layout-aware, and null for any key with no printable form -- arrows,
    // function keys, modifiers.  Those emit an empty string, which is what
    // XLookupString yields for them too.
    const char* name = glfwGetKeyName(key, scancode);
    if(name == 0)
        return std::string();

    std::string text(name);

    // glfwGetKeyName reports the unshifted character; X reports what was
    // actually typed.
    if((mods & GLFW_MOD_SHIFT) != 0) {
        for(std::string::size_type i = 0; i < text.size(); i++) {
            text[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
        }
    }

    return text;
}

void Window::on_key(GLFWwindow* w, int key, int scancode, int action, int mods) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if(self == 0)
        return;

    double x = 0, y = 0;
    glfwGetCursorPos(w, &x, &y);

    const std::string text = key_text(key, scancode, mods);

    // GLFW_REPEAT is reported as a press, matching X's auto-repeat.
    if(action == GLFW_PRESS || action == GLFW_REPEAT)
        self->key_press.emit(text, static_cast<int>(x), static_cast<int>(y));
    else if(action == GLFW_RELEASE)
        self->key_release.emit(text, static_cast<int>(x), static_cast<int>(y));
}

void Window::on_button(GLFWwindow* w, int button, int action, int mods) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if(self == 0)
        return;

    double x = 0, y = 0;
    glfwGetCursorPos(w, &x, &y);

    // X numbers buttons from 1; GLFW from 0.  The apps were written against
    // X's numbering.
    const int n = button + 1;

    if(action == GLFW_PRESS)
        self->button_press.emit(n, static_cast<int>(x), static_cast<int>(y));
    else if(action == GLFW_RELEASE)
        self->button_release.emit(n, static_cast<int>(x), static_cast<int>(y));
}

void Window::on_framebuffer_size(GLFWwindow* w, int width, int height) {
    Window* self = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if(self == 0)
        return;

    // Framebuffer size rather than window size, so consumers can hand this
    // straight to glViewport.
    self->configure_notify.emit(width, height);
}

}
}
