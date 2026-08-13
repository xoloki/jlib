AM_CPPFLAGS = -I$(top_srcdir) $(GLFW_CFLAGS) $(GL_CFLAGS) $(GLU_CFLAGS)

if HAVE_GLFW
LIBJGLFW = libjglfw.la
endif

lib_LTLIBRARIES = $(LIBJGLFW)
libjglfw_la_SOURCES = Window.cc
libjglfw_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjglfw_la_LIBADD = $(top_builddir)/jlib/gl/libjgl.la \
                     $(top_builddir)/jlib/glu/libjglu.la \
                     $(GLFW_LIBS) $(GLU_LIBS) $(GL_LIBS)

jglfw_hdrs = Window.hh Plot.hh

libjglfwincludedir = $(includedir)/jlib-1.2/jlib/glfw
if HAVE_GLFW
libjglfwinclude_HEADERS = $(jglfw_hdrs)
endif

EXTRA_DIST = $(jglfw_hdrs)
