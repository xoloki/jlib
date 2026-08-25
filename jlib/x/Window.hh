/* -*- mode: C++ c-basic-offset: 4 -*-
 *
 * Copyright (c) 1999 Joey Yandle <xoloki@gmail.com>
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

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <string>

#include <jlib/sys/signal.hh>

namespace jlib {
namespace x {
        
class Window {
public:
    typedef enum { DRAW, ERASE, OVERLAY, INVERT } mode_type;
    
    Window(const std::string& title, int w, int h);
    virtual ~Window();
    
    void set_mode(mode_type mode);
    mode_type get_mode() const;
    
    void set_auto_flush(bool auto_flush);
    bool is_auto_flush() const;
    
    void set_foreground(int r, int g, int b);
    void set_foreground(unsigned long c);
    
    int get_width() const;
    int get_height() const;
    
    std::pair<int,int> get_p() const;
    int get_x() const;
    int get_y() const;
    
    virtual void clear();
    
    void center();
    
    void move(int x, int y);
    void move(std::pair<int,int> p);
    
    void seek(int x, int y);
    void seek(std::pair<int,int> p);
    
    void draw_point();
    void draw_point(int x, int y);
    void draw_point(std::pair<int,int> p);
    
    void draw_string(int s);
    void draw_string(const std::string& s);
    void draw_string(std::pair<int,int> p, int s);
    void draw_string(std::pair<int,int> p, const std::string& s);
    
    void draw_line(int x, int y);
    void draw_line(std::pair<int,int> p);
    void draw_line(int x0, int y0, int x1, int y1);
    void draw_line(std::pair<int,int> p0, std::pair<int,int> p1);
    
    void draw_rectangle(int w, int h);
    void draw_rectangle(std::pair<int,int> size);
    void draw_rectangle(std::pair<int,int> p, int w, int h);
    void draw_rectangle(std::pair<int,int> p, std::pair<int,int> size);
    void draw_rectangle(int x, int y, int w, int h);
    void draw_rectangle(int x, int y, std::pair<int,int> size);
    
    void fill_rectangle(int w, int h);
    void fill_rectangle(std::pair<int,int> size);
    void fill_rectangle(std::pair<int,int> p, int w, int h);
    void fill_rectangle(std::pair<int,int> p, std::pair<int,int> size);
    void fill_rectangle(int x, int y, int w, int h);
    void fill_rectangle(int x, int y, std::pair<int,int> size);
    
    void draw_oval(int w, int h);
    void draw_oval(std::pair<int,int> size);
    void draw_oval(std::pair<int,int> p, int w, int h);
    void draw_oval(std::pair<int,int> p, std::pair<int,int> size);
    void draw_oval(int x, int y, int w, int h);
    void draw_oval(int x, int y, std::pair<int,int> size);
    
    void fill_oval(int w, int h);
    void fill_oval(std::pair<int,int> size);
    void fill_oval(std::pair<int,int> p, int w, int h);
    void fill_oval(std::pair<int,int> p, std::pair<int,int> size);
    void fill_oval(int x, int y, int w, int h);
    void fill_oval(int x, int y, std::pair<int,int> size);
    
    void set_line_size(int pixels);
    
    void select_input(long event_mask);
    void set_font(const std::string& font);
    void set_title(const std::string& title);

    void set_timeout(long micro);
    
    virtual void flush();
    
    int pending();
    void run();
    virtual void iterate();
    
    Window& operator<<(const std::string& msg);
    Window& operator<<(int value);
    
    sys::signal<void(std::string,int,int)> key_press;
    sys::signal<void(std::string,int,int)> key_release;
    
    sys::signal<void(int,int,int)> button_press;
    sys::signal<void(int,int,int)> button_release;

    sys::signal<void(int,int)> configure_notify;

    sys::signal<void()> timeout;
    
protected:
    Window();

    void init_gc();
    
    mode_type m_mode;
    std::string m_title;
    std::pair<int,int> m_p;
    int m_width;
    int m_height;
    
    int m_red_shift;
    int m_green_shift;
    int m_blue_shift;
    
    bool m_auto_flush;
    
    long m_event_mask;
    long m_timeout;

    Display* m_dpy;
    ::Window m_win;
    Visual* m_visual;
    int	m_screen;
    GC m_gc_draw;
    GC m_gc_erase;
    GC m_gc_invert;
    GC m_gc_overlay;
    GC m_gc;
    XGCValues m_gc_values;
    XFontStruct* m_font;
    XSetWindowAttributes m_swa;
};
    
}
}

/*
  
void Arc(const Rect& R, int startangle=0, int sweepangle=90);
void PaintArc(const Rect& R, int startangle=0, int sweepangle=90);
};

*/
