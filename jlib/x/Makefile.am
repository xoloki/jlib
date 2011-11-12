INCLUDES = -I$(top_srcdir)

if HAVE_X
LIBJX = libjx.la
else
LIBJX = 
endif

lib_LTLIBRARIES = $(LIBJX)
libjx_la_SOURCES = Window.cc Display.cc
libjx_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined -L/usr/X11R6/lib -lX11 -lXext
libjxincludedir=$(includedir)/jlib-1.2/jlib/x

libjxinclude_HEADERS = Window.hh Plot.hh Display.hh

