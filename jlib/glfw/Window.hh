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

#ifndef JLIB_GLFW_WINDOW_HH
#define JLIB_GLFW_WINDOW_HH

#include <jlib/gl/opengl.hh>
#include <jlib/sys/signal.hh>

#include <exception>
#include <string>

struct GLFWwindow;

namespace jlib {
namespace glfw {

/**
 * A window with an OpenGL context, over GLFW.
 *
 * This replaces both glx::Window and the glut::Main singleton.  GLX exists
 * only on X11, and GLUT is deprecated on macOS and X11-only in practice
 * elsewhere; GLFW runs on X11, Wayland and Cocoa alike.
 *
 * The event surface deliberately matches jlib::x::Window's -- the same six
 * signals with the same signatures -- so Hyper.hh and the apps do not care
 * which backend they are on.  Two consequences of GLFW's shape are worth
 * knowing:
 *
 *  - GLFW hands back the main loop rather than never returning, so run() is a
 *    real loop and iterate() can be driven externally, the way jpoisoned
 *    drives x::Window.  glutMainLoop() could do neither, which is why
 *    glut::Main had to publish process-global static signals and jglfwhyper
 *    had to bind to them instead of to its own object.
 *
 *  - Every GLFW window and event call must happen on the main thread.  That
 *    is Cocoa's requirement, and GLFW enforces it everywhere for consistency.
 *
 * The context is deliberately a legacy one: jlib's rendering is fixed-function
 * OpenGL 1.1/1.2 -- glBegin, the matrix stack, glLightfv, glTexGeni, GLU --
 * and macOS offers a 2.1 compatibility context or a 3.2+ core context with
 * nothing in between.  Requesting core would kill every one of those calls at
 * once, and there is no fallback path.  So no GLFW_OPENGL_PROFILE hint is set.
 */
class Window {
public:
    class exception : public std::exception {
    public:
        exception(const std::string& msg = "") {
            m_msg = "jlib::glfw::Window exception" + (msg != "" ? (": " + msg) : "");
        }
        virtual ~exception() noexcept {}
        virtual const char* what() const noexcept { return m_msg.c_str(); }

    protected:
        std::string m_msg;
    };

    Window(const std::string& title, int width, int height, bool depth = true);
    virtual ~Window();

    /**
     * Framebuffer size, not window size.
     *
     * These differ on a scaled display -- 2x on Retina -- and glViewport
     * wants the framebuffer.  Passing the window size there renders into a
     * quarter of the window.
     */
    int get_width() const;
    int get_height() const;

    void set_title(const std::string& title);
    void center();

    void clear();

    /**
     * Swap buffers.  Named to match x::Window, where it flushes the X queue.
     */
    void flush();

    bool should_close() const;
    void set_should_close(bool close = true);

    /**
     * Drain pending events, then fire timeout.
     */
    virtual void iterate();

    /**
     * iterate() until the window is closed.
     */
    void run();

    /**
     * Microseconds to wait in iterate() before firing timeout.
     */
    void set_timeout(long micro);
    long get_timeout() const;

    void make_current();

    GLFWwindow* get_handle();

    sys::signal<void(std::string,int,int)> key_press;
    sys::signal<void(std::string,int,int)> key_release;

    sys::signal<void(int,int,int)> button_press;
    sys::signal<void(int,int,int)> button_release;

    sys::signal<void(int,int)> configure_notify;

    sys::signal<void()> timeout;

protected:
    /**
     * GLFW callbacks are plain C function pointers, so they recover the
     * Window through glfwGetWindowUserPointer.  That is what lets this class
     * publish per-object signals where glut::Main had to use statics.
     */
    static void on_key(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void on_button(GLFWwindow* w, int button, int action, int mods);
    static void on_framebuffer_size(GLFWwindow* w, int width, int height);

    /**
     * The text a key produces, matching what x::Window emits.
     *
     * x::Window sends the result of XLookupString -- the typed characters,
     * not a keysym name -- so "q" arrives as "q" and space as " ".  Keys that
     * produce no text, arrows among them, yield an empty string there and
     * here.
     */
    static std::string key_text(int key, int scancode, int mods);

    GLFWwindow* m_window;
    std::string m_title;
    long m_timeout;

    /**
     * Whether the synthetic first configure has been delivered.  GLFW sends
     * no resize event for a window's initial size, so without this a plot
     * never learns its own dimensions until the user happens to resize it.
     */
    bool m_configured;
};

}
}

#endif //JLIB_GLFW_WINDOW_HH
