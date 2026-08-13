AM_CPPFLAGS = -I$(top_srcdir) $(GL_CFLAGS)

if HAVE_GL
LIBJGL = libjgl.la
endif

lib_LTLIBRARIES = $(LIBJGL)
libjgl_la_SOURCES = lights.cc buffers.cc
libjgl_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjgl_la_LIBADD = $(GL_LIBS)

jgl_hdrs = lights.hh buffers.hh opengl.hh

libjglincludedir = $(includedir)/jlib-1.2/jlib/gl
if HAVE_GL
libjglinclude_HEADERS = $(jgl_hdrs)
endif

EXTRA_DIST = $(jgl_hdrs)
