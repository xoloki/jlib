AM_CPPFLAGS = -I$(top_srcdir) $(GL_CFLAGS) $(GLU_CFLAGS)

if HAVE_GLU
LIBJGLU = libjglu.la
endif

lib_LTLIBRARIES = $(LIBJGLU)
libjglu_la_SOURCES = textures.cc projection.cc
libjglu_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjglu_la_LIBADD = $(GLU_LIBS) $(GL_LIBS)

jglu_hdrs = textures.hh projection.hh

libjgluincludedir = $(includedir)/jlib-1.2/jlib/glu
if HAVE_GLU
libjgluinclude_HEADERS = $(jglu_hdrs)
endif

EXTRA_DIST = $(jglu_hdrs)
