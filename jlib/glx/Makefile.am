INCLUDES = -I$(top_srcdir)

if HAVE_GLX
LIBJGLX = libjglx.la
else
LIBJGLX = 
endif

lib_LTLIBRARIES = $(LIBJGLX)
libjglx_la_SOURCES = Window.cc
libjglx_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined -L/usr/X11/lib -lX11 -lXext -lGL -lGLU
libjglx_la_LIBADD = $(top_builddir)/jlib/x/libjx.la $(top_builddir)/jlib/gl/libjgl.la $(top_builddir)/jlib/glu/libjglu.la
libjglxincludedir=$(includedir)/jlib-1.2/jlib/glx

libjglxinclude_HEADERS = Window.hh Plot.hh


