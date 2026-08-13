AM_CPPFLAGS = -I$(top_srcdir) $(X11_CFLAGS)

if HAVE_X
LIBJX = libjx.la
endif

lib_LTLIBRARIES = $(LIBJX)
libjx_la_SOURCES = Window.cc Display.cc
libjx_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjx_la_LIBADD = $(X11_LIBS)

jx_hdrs = Window.hh Plot.hh Display.hh

libjxincludedir = $(includedir)/jlib-1.2/jlib/x
if HAVE_X
libjxinclude_HEADERS = $(jx_hdrs)
endif

EXTRA_DIST = $(jx_hdrs)
