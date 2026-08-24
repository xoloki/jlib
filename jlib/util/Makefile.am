AM_CPPFLAGS = -I$(top_srcdir) $(JSONC_CFLAGS)

lib_LTLIBRARIES = libjutil.la
libjutil_la_SOURCES = abnf.cc Regex.cc MimeType.cc util.cc Date.cc xml.cc \
                      URL.cc Headers.cc Timer.cc json.cc
libjutil_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjutil_la_LIBADD = $(top_builddir)/jlib/sys/libjsys.la $(JSONC_LIBS)

libjutilincludedir = $(includedir)/jlib-1.2/jlib/util
libjutilinclude_HEADERS = abnf.hh Regex.hh MimeType.hh util.hh Date.hh xml.hh \
                          URL.hh Headers.hh Timer.hh json.hh
