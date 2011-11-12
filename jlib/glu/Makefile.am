INCLUDES = -I$(top_srcdir)

if HAVE_GLU
LIBJGLU = libjglu.la
else
LIBJGLU = 
endif

lib_LTLIBRARIES = $(LIBJGLU)
libjglu_la_SOURCES = textures.cc projection.cc
libjglu_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined -L/usr/X11/lib -lX11 -lXext -lGL -lGLU
libjglu_la_LIBADD = $(top_builddir)/jlib/x/libjx.la
libjgluincludedir=$(includedir)/jlib-1.2/jlib/glu

libjgluinclude_HEADERS = textures.hh projection.hh

