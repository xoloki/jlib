AM_CPPFLAGS = -I$(top_srcdir) $(OPENSSL_CFLAGS) $(REACTOR_CPPFLAGS)

lib_LTLIBRARIES = libjsys.la
libjsys_la_SOURCES = tfstream.cc sys.cc Directory.cc Servent.cc pipe.cc listener.cc \
                     tls.cc server.cc \
                     reactor.cc reactor_backend.cc reactor_poll.cc \
                     async_reader.cc async_writer.cc async_tls.cc
libjsys_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
# tls.cc is the first OpenSSL .cc in libjsys -- sslstream.hh is header-only,
# so until now the flags were enough and the libraries were not needed.
libjsys_la_LIBADD = $(PTHREAD_LIBS) $(OPENSSL_LIBS)

libjsysincludedir = $(includedir)/jlib-$(JLIB_RELEASE)/jlib/sys
libjsysinclude_HEADERS = tfstream.hh socketstream.hh sslstream.hh proxystream.hh \
                         listener.hh tls.hh server.hh reactor.hh task.hh await.hh async_reader.hh async_writer.hh async_tls.hh \
                         sslproxystream.hh serialstream.hh pstream.hh \
                         sys.hh Directory.hh auto.hh sync.hh ringbuffer.hh Servent.hh \
                         ASServent.hh pipe.hh relay.hh object.hh joystick.hh \
                         signal.hh

if HAVE_KQUEUE
libjsys_la_SOURCES += reactor_kqueue.cc
endif

if HAVE_EPOLL
libjsys_la_SOURCES += reactor_epoll.cc
endif

# Not installed: which backend is running is not part of the contract, and
# reactor::backend() is all a caller may ask.
EXTRA_DIST = reactor_backend.hh
