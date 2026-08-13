AM_CPPFLAGS = -I$(top_srcdir) $(GL_CFLAGS) $(GLU_CFLAGS) $(GLUT_CFLAGS)

if HAVE_GLUT
LIBJGLUT = libjglut.la
endif

lib_LTLIBRARIES = $(LIBJGLUT)
libjglut_la_SOURCES = main.cc
libjglut_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjglut_la_LIBADD = $(top_builddir)/jlib/gl/libjgl.la \
                     $(top_builddir)/jlib/glu/libjglu.la \
                     $(GLUT_LIBS) $(GLU_LIBS) $(GL_LIBS)

jglut_hdrs = main.hh Plot.hh glut.hh

libjglutincludedir = $(includedir)/jlib-1.2/jlib/glut
if HAVE_GLUT
libjglutinclude_HEADERS = $(jglut_hdrs)
endif

EXTRA_DIST = $(jglut_hdrs)
