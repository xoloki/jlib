AM_CPPFLAGS = -I$(top_srcdir) $(OPENSSL_CFLAGS)

lib_LTLIBRARIES = libjsys.la
libjsys_la_SOURCES = tfstream.cc sys.cc Directory.cc Servent.cc pipe.cc listener.cc \
                     tls.cc server.cc
libjsys_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
# tls.cc is the first OpenSSL .cc in libjsys -- sslstream.hh is header-only,
# so until now the flags were enough and the libraries were not needed.
libjsys_la_LIBADD = $(PTHREAD_LIBS) $(OPENSSL_LIBS)

libjsysincludedir = $(includedir)/jlib-1.2/jlib/sys
libjsysinclude_HEADERS = tfstream.hh socketstream.hh sslstream.hh proxystream.hh \
                         listener.hh tls.hh server.hh \
                         sslproxystream.hh serialstream.hh pstream.hh \
                         sys.hh Directory.hh auto.hh sync.hh ringbuffer.hh Servent.hh \
                         ASServent.hh pipe.hh object.hh joystick.hh
