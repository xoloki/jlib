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

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <string>

#include <sigc++/sigc++.h>

namespace jlib {
namespace x {
        
class Window {
public:
    typedef enum { DRAW, ERASE, OVERLAY, INVERT } mode_type;
    
    Window(std::string title, int w, int h);
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
    void draw_string(std::string s);
    void draw_string(std::pair<int,int> p, int s);
    void draw_string(std::pair<int,int> p, std::string s);
    
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
    void set_font(std::string font);
    void set_title(std::string title);

    void set_timeout(long micro);
    
    virtual void flush();
    
    int pending();
    void run();
    virtual void iterate();
    
    Window& operator<<(std::string msg);
    Window& operator<<(int value);
    
    sigc::signal<void,std::string,int,int> key_press;
    sigc::signal<void,std::string,int,int> key_release;
    
    sigc::signal<void,int,int,int> button_press;
    sigc::signal<void,int,int,int> button_release;

    sigc::signal<void,int,int> configure_notify;

    sigc::signal<void> timeout;
    
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
