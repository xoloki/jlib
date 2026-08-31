AM_CPPFLAGS = -I$(top_srcdir)

lib_LTLIBRARIES = libjai.la
libjai_la_SOURCES = environment.cc agent.cc percept.cc action.cc vacuum.cc \
                    backend.cc
libjai_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjai_la_LIBADD = $(top_builddir)/jlib/util/libjutil.la \
                   $(top_builddir)/jlib/sys/libjsys.la

libjaiincludedir = $(includedir)/jlib-1.2/jlib/ai
libjaiinclude_HEADERS = environment.hh agent.hh percept.hh action.hh vacuum.hh \
                        neural.hh backend.hh attention.hh transformer.hh
