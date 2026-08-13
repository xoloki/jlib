AM_CPPFLAGS = -I$(top_srcdir) $(GL_CFLAGS) $(GLU_CFLAGS) $(X11_CFLAGS)

if HAVE_GLX
LIBJGLX = libjglx.la
endif

lib_LTLIBRARIES = $(LIBJGLX)
libjglx_la_SOURCES = Window.cc
libjglx_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjglx_la_LIBADD = $(top_builddir)/jlib/x/libjx.la \
                    $(top_builddir)/jlib/gl/libjgl.la \
                    $(top_builddir)/jlib/glu/libjglu.la \
                    $(GLU_LIBS) $(GL_LIBS) $(X11_LIBS)

jglx_hdrs = Window.hh Plot.hh

libjglxincludedir = $(includedir)/jlib-1.2/jlib/glx
if HAVE_GLX
libjglxinclude_HEADERS = $(jglx_hdrs)
endif

EXTRA_DIST = $(jglx_hdrs)
