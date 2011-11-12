INCLUDES = -I$(top_srcdir)

if HAVE_GL
LIBJGL = libjgl.la
else
LIBJGL = 
endif

lib_LTLIBRARIES = $(LIBJGL)
libjgl_la_SOURCES = lights.cc buffers.cc
libjgl_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined -L/usr/X11/lib -lX11 -lXext -lGL
libjgl_la_LIBADD = $(top_builddir)/jlib/x/libjx.la
libjglincludedir=$(includedir)/jlib-1.2/jlib/gl

libjglinclude_HEADERS = lights.hh buffers.hh

