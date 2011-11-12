INCLUDES = -I$(top_srcdir)

if HAVE_GLUT
LIBJGLUT = libjglut.la
else
LIBJGLUT = 
endif

lib_LTLIBRARIES = $(LIBJGLUT)
libjglut_la_SOURCES = main.cc
libjglut_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined -L/usr/X11/lib -lX11 -lXext -lGL -lGLU -lglut
libjglut_la_LIBADD = $(top_builddir)/jlib/x/libjx.la $(top_builddir)/jlib/gl/libjgl.la $(top_builddir)/jlib/glu/libjglu.la
libjglutincludedir=$(includedir)/jlib-1.2/jlib/glut

libjglutinclude_HEADERS = main.hh Plot.hh


