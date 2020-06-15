INCLUDES = -I$(top_srcdir)

lib_LTLIBRARIES = libjutil.la
libjutil_la_SOURCES = Regex.cc MimeType.cc util.cc Date.cc xml.cc xmlparser.cc \
                       xmltokenizer.cc URL.cc Headers.cc Timer.cc json.cc
libjutil_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjutil_la_LIBADD = $(top_builddir)/jlib/sys/libjsys.la
libjutilincludedir=$(includedir)/jlib-1.2/jlib/util

libjutilinclude_HEADERS = Regex.hh MimeType.hh util.hh Date.hh xml.hh xmlparser.hh \
                     xmltokenizer.hh URL.hh Headers.hh Timer.hh json.hh

