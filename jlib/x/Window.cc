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

#include <jlib/x/Window.hh>
#include <glibmm/main.h>
#include <glibmm/thread.h>
#include <glibmm/timer.h>

#include <iostream>
#include <sstream>

namespace jlib {
namespace x {

Window::Window()
    : m_timeout(1000),
      m_auto_flush(false)
{
}

Window::Window(std::string title, int w, int h) 
{
    m_timeout = 1000;

    m_width = w;
    m_height = h;
    
    m_auto_flush = true;
    
    m_dpy = XOpenDisplay(NULL);
    m_screen = DefaultScreen(m_dpy);
    m_visual = DefaultVisual(m_dpy, m_screen);
    
    m_swa.backing_store    = WhenMapped;
    m_swa.backing_planes   = 0xffffffff;
    m_swa.backing_pixel    = WhitePixel(m_dpy,m_screen);
    m_swa.background_pixel = BlackPixel(m_dpy,m_screen);
    m_swa.border_pixel     = BlackPixel(m_dpy,m_screen);
    unsigned long values = (CWBackingStore|CWBackingPlanes|
                            CWBackingPixel|CWBackPixel|CWBorderPixel);
    
    m_win = XCreateWindow(m_dpy, 
                          RootWindow(m_dpy,m_screen), 
                          0, 0, 
                          w, h, 
                          0, 
                          CopyFromParent,
                          InputOutput,
                          CopyFromParent,
                          values,
                          &m_swa);
    
    select_input(ExposureMask | 
                 StructureNotifyMask | 
                 KeyPressMask | 
                 ButtonPressMask | 
                 ButtonReleaseMask);
    
    set_title(title);
    
    XMapWindow(m_dpy, m_win);
    
    XEvent evt;
    bool bunny = true;
    while(bunny) {
		XNextEvent(m_dpy,&evt);
		if(evt.type == Expose ) bunny = false;
    }
    
    init_gc();
    set_mode(DRAW);
    clear();
    move(0,0);
}
    
Window::~Window() {}
    
void Window::init_gc() {
    m_gc_values.foreground = WhitePixel(m_dpy,m_screen);
    m_gc_values.background = BlackPixel(m_dpy,m_screen);
    m_gc_values.function   = GXcopy;
    m_gc_draw  = XCreateGC(m_dpy,m_win,GCForeground | GCBackground | GCFunction,&m_gc_values);
    m_gc_values.foreground = BlackPixel(m_dpy,m_screen);
    m_gc_erase  = XCreateGC(m_dpy,m_win,GCForeground | GCBackground | GCFunction,&m_gc_values);
    m_gc_values.function = GXinvert;
    m_gc_invert  = XCreateGC(m_dpy,m_win,GCForeground | GCBackground | GCFunction,&m_gc_values);
    m_gc_overlay  = XCreateGC(m_dpy,m_win,GCForeground | GCBackground | GCFunction,&m_gc_values);
    
    set_font("-misc-fixed-medium-r-normal--13-120-75-75-c-70-iso8859-1");
    
    u_long mask;
    int i;
    
    mask = m_visual->red_mask;
    i = 0;
    if(mask) {
		while(!(mask&1)) {
		    mask = mask>>1;
		    i++;
		}
    }
    m_red_shift = i;
    
    mask = m_visual->green_mask;
    i = 0;
    if(mask) {
		while(!(mask&1)) {
		    mask = mask>>1;
		    i++;
		}
    }
    m_green_shift = i;
    
    mask = m_visual->blue_mask;
    i = 0;
    if(mask) {
		while(!(mask&1)) {
		    mask = mask>>1;
		    i++;
		}
    }
    m_blue_shift = i;
}
	
    
// Change the drawing mode to m (DRAW, OVERLAY, ERASE, INVERT)
void Window::set_mode(mode_type m) {
    m_mode = m;
    switch (m) {
    case DRAW:  
		m_gc = m_gc_draw;
		break;
    case ERASE: 
		m_gc = m_gc_erase;
		break;
    case OVERLAY: 
		m_gc = m_gc_overlay;
		break;
    case INVERT: 
		m_gc = m_gc_invert;
		break;
    }
}
    
Window::mode_type Window::get_mode() const {
    return m_mode;
}
	
void Window::clear() {
    XClearWindow(m_dpy,m_win);

    if(m_auto_flush)
        XFlush(m_dpy);
}
	
void Window::center() {
    move(get_width()/2, get_height()/2);
}
    
// Set the pen size to (npix x npix).
// Only square pens are supported.
void Window::set_line_size(int pixels) {
    XSetLineAttributes(m_dpy, m_gc, unsigned(pixels), LineSolid, CapRound, JoinBevel);
}
	
void Window::draw_string(std::pair<int,int> p, int s) {
    move(p);
    draw_string(s);
}
	
void Window::draw_string(std::pair<int,int> p, std::string s) {
    move(p);
    draw_string(s);
}
	
void Window::draw_string(std::string s) {
    XDrawString(m_dpy, m_win, m_gc, m_p.first, m_p.second, s.data(), s.length());
    int w = XTextWidth(m_font, s.data(), s.length());
    move(m_p.first+w,m_p.second);
	
    if(m_auto_flush)
        XFlush(m_dpy);
}
	
void Window::draw_string(int s) {
    std::ostringstream o;
    o << s;
    draw_string(o.str());
}
	
Window& Window::operator<<(std::string msg) {
    draw_string(msg);
    return *this;
}
	
Window& Window::operator<<(int s) {
    draw_string(s);
    return *this;
}
	
void Window::move(int x, int y) {
    m_p.first = x;
    m_p.second = y;
}
	
void Window::move(std::pair<int,int> p) {
    move(p.first,p.second);
}
	
void Window::seek(int x, int y) {
    m_p.first += x;
    m_p.second += y;
}
	
void Window::seek(std::pair<int,int> p) {
    seek(p.first,p.second);
}
	
void Window::draw_point() {
    XDrawPoint(m_dpy, m_win, m_gc, m_p.first, m_p.second);
    if(m_auto_flush)
		XFlush(m_dpy);
}

void Window::draw_point(int x, int y) {
    move(x,y);
    draw_point();
}

void Window::draw_point(std::pair<int,int> p) {
    move(p);
    draw_point();
}
	
	
void Window::draw_line(int x, int y) {
    XDrawLine(m_dpy, m_win, m_gc, m_p.first, m_p.second, x, y);
    if(m_auto_flush)
		XFlush(m_dpy);
}
	
void Window::draw_line(std::pair<int,int> p) {
    draw_line(p.first, p.second);
}
    
void Window::draw_line(int x0, int y0, int x1, int y1) {
    move(x0,y0);
    draw_line(x1,y1);
}
	
void Window::draw_line(std::pair<int,int> p0, std::pair<int,int> p1) {
    draw_line(p0.first, p0.second, p1.first, p1.second);
}
    
	
void Window::draw_rectangle(int w, int h) {
    XDrawRectangle(m_dpy, m_win, m_gc, m_p.first, m_p.second, w, h);
    if(m_auto_flush)
		XFlush(m_dpy);
}
	
void Window::draw_rectangle(std::pair<int,int> size) {
    draw_rectangle(size.first, size.second);
}
	
void Window::draw_rectangle(std::pair<int,int> p, int w, int h) {
    move(p);
    draw_rectangle(w,h);
}
	
void Window::draw_rectangle(std::pair<int,int> p, std::pair<int,int> size) {
    move(p);
    draw_rectangle(size.first, size.second);
}
	
void Window::draw_rectangle(int x, int y, int w, int h) {
    move(x,y);
    draw_rectangle(w,h);
}
	
void Window::draw_rectangle(int x, int y, std::pair<int,int> size) {
    move(x,y);
    draw_rectangle(size.first, size.second);
}
	
	
void Window::fill_rectangle(int w, int h) {
    XFillRectangle(m_dpy, m_win, m_gc, m_p.first, m_p.second, w, h);
    if(m_auto_flush)
		XFlush(m_dpy);
}
	
void Window::fill_rectangle(std::pair<int,int> size) {
    fill_rectangle(size.first, size.second);
}
    
void Window::fill_rectangle(std::pair<int,int> p, int w, int h) {
    move(p);
    fill_rectangle(w,h);
}
	
void Window::fill_rectangle(std::pair<int,int> p, std::pair<int,int> size) {
    move(p);
    fill_rectangle(size.first, size.second);
}
	
void Window::fill_rectangle(int x, int y, int w, int h) {
    move(x,y);
    fill_rectangle(w,h);
}
	
void Window::fill_rectangle(int x, int y, std::pair<int,int> size) {
    move(x,y);
    fill_rectangle(size.first, size.second);
}
	
	
	
void Window::draw_oval(int w, int h) {
    XDrawArc( m_dpy, m_win, m_gc, m_p.first, m_p.second, w, h, 90*64, 360*64 );
    if(m_auto_flush)
		XFlush(m_dpy);
}
	
void Window::draw_oval(std::pair<int,int> size) {
    draw_oval(size.first, size.second);
}
	
void Window::draw_oval(std::pair<int,int> p, int w, int h) {
    move(p);
    draw_oval(w,h);
}
	
void Window::draw_oval(std::pair<int,int> p, std::pair<int,int> size) {
    move(p);
    draw_oval(size.first, size.second);
}
	
void Window::draw_oval(int x, int y, int w, int h) {
    move(x,y);
    draw_oval(w,h);
}
	
void Window::draw_oval(int x, int y, std::pair<int,int> size) {
    move(x,y);
    draw_oval(size.first, size.second);
}
	
	
void Window::fill_oval(int w, int h) {
    XFillArc( m_dpy, m_win, m_gc, m_p.first, m_p.second, w, h, 90*64, 360*64 );
    if(m_auto_flush)
		XFlush(m_dpy);
}
	
void Window::fill_oval(std::pair<int,int> size) {
    fill_oval(size.first, size.second);
}
    
void Window::fill_oval(std::pair<int,int> p, int w, int h) {
    move(p);
    fill_oval(w,h);
}
	
void Window::fill_oval(std::pair<int,int> p, std::pair<int,int> size) {
    move(p);
    fill_oval(size.first, size.second);
}
	
void Window::fill_oval(int x, int y, int w, int h) {
    move(x,y);
    fill_oval(w,h);
}
	
void Window::fill_oval(int x, int y, std::pair<int,int> size) {
    move(x,y);
    fill_oval(size.first, size.second);
}
	
void Window::set_foreground(int r, int g, int b) {
    XSetForeground(m_dpy, m_gc, (r<<m_red_shift)|(g<<m_green_shift)|(b<<m_blue_shift));
}
    
void Window::set_foreground(unsigned long c) {
    XSetForeground(m_dpy, m_gc, c);
}
	
int Window::get_width() const { return m_width; }
int Window::get_height() const { return m_height; }
	
void Window::set_auto_flush(bool auto_flush) { m_auto_flush = auto_flush; }
bool Window::is_auto_flush() const { return m_auto_flush; }
	
void Window::flush() { 
    XFlush(m_dpy);
}
	
void Window::select_input(long event_mask) {
    m_event_mask = event_mask;
    XSelectInput(m_dpy, m_win, m_event_mask);
}
	
void Window::set_font(std::string font) {
    m_font=XLoadQueryFont(m_dpy,font.c_str());
	
    XSetFont(m_dpy,m_gc_draw,m_font->fid);
    XSetFont(m_dpy,m_gc_erase,m_font->fid);
    XSetFont(m_dpy,m_gc_invert,m_font->fid);
    XSetFont(m_dpy,m_gc_overlay,m_font->fid);
}
    
void Window::set_title(std::string title) {
    m_title=title;
    XSetStandardProperties(m_dpy, m_win, title.c_str(), title.c_str(), 
                           None, NULL,0, NULL);
}
	
int Window::pending() {
    return XPending(m_dpy);
}
	
void Window::run() {
    while(true) {
		iterate();
    }
}
	
void Window::iterate() {
    const int sz = 64;
    int n;
    char buffer[sz];
    KeySym sym;
    XEvent event;
	
    while(pending()) {
		XNextEvent(m_dpy,&event);
		switch(event.type) {
		case KeyPress:
		    n = XLookupString(&(event.xkey),buffer,sz,&sym,0);
		    key_press.emit(std::string(buffer,n), event.xkey.x, event.xkey.y);
		    break;
		case KeyRelease:
		    n = XLookupString(&(event.xkey),buffer,sz,&sym,0);
		    key_release.emit(std::string(buffer,n), event.xkey.x, event.xkey.y);
		    break;
		case ButtonPress:
		    button_press.emit(event.xbutton.button, event.xbutton.x, event.xbutton.y);
		    break;
		case ButtonRelease:
		    button_release.emit(event.xbutton.button, event.xbutton.x, event.xbutton.y);
		    break;
		case ConfigureNotify:
		    m_width = event.xconfigure.width;
		    m_height = event.xconfigure.height;
		    configure_notify.emit(m_width, m_height);
		    break;
		}
    }

    Glib::usleep(m_timeout);
    timeout.emit();
}

void Window::set_timeout(long micro) {
    m_timeout = micro;
}
	
	/*
	  
	// Draw the frame of an arc; 0 degrees is up; 
	// positive angles increase clockwise;
	// negative angles are counterclockwise
	void Window::Arc(const IPLrect& R, int startangle, int sweepangle) {
	int height = R.Bottom() - R.Top();
	int width  = R.Right() - R.Left();
	XDrawArc( m_dpy, win, gc, R.Left(), R.Top(), 
	width, height, 90*64-startangle*64, -sweepangle*64 );
	}
	
	// Paint an arc; 0 degrees is up; positive angles increase clockwise;
	// negative angles are counterclockwise
	void Window::PaintArc(const IPLrect& R, int startangle, int sweepangle) {
	int height = R.Bottom() - R.Top();
	int width  = R.Right() - R.Left();
	XFillArc( m_dpy, win, gc, R.Left(), R.Top(), 
	width, height, 90*64-startangle*64, -sweepangle*64 );
	}
	
	
	bool predfunc( Display* display, XEvent* event, char* arg ) {
	return ( event->type == KeyPress && 
	XKeycodeToKeysym(display, event->xkey.keycode, 0) == unsigned(*arg) );
	}
	
	bool check_key( char key ) {
	XEvent event;
	return XCheckIfEvent( m_dpy, &event, predfunc, &key );
	}
	
	char check_key() {
	char keys[32];
	int bt = -1;
	XQueryKeymap( m_dpy, keys );
	for( int i = 0; i < 32; i++ ) 
	if( keys[i] != 0 ) {
	bt = i;
	}
	if( bt == -1 ) return 0;
	int key = 8*bt;
	
	switch( keys[bt] ) {
	case 1: 
	key += 0; 
	break;
	case 2: 
	key += 1; 
	break;
	case 4: 
	key += 2; 
	break;
	case 8: 
	key += 3; 
	break;
	case 16: 
	key += 4; 
	break;
	case 32: 
	key += 5; 
	break;
	case 64: 
	key += 6; 
	break;
	case -128: 
	key += 7; 
	break;
	default:
	break;
	}
	
	char code = XKeycodeToKeysym(m_dpy,key,0);
	
	return code;
	}
	*/
	
    }
}
