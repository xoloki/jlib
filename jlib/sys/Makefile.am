INCLUDES = -I$(top_srcdir)

lib_LTLIBRARIES = libjsys.la
libjsys_la_SOURCES = tfstream.cc sys.cc Directory.cc Servent.cc pipe.cc 
libjsys_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined -lpthread
libjsysincludedir=$(includedir)/jlib-1.2/jlib/sys

libjsysinclude_HEADERS = tfstream.hh socketstream.hh sslstream.hh proxystream.hh \
                         sslproxystream.hh serialstream.hh pstream.hh \
                         sys.hh Directory.hh auto.hh sync.hh Servent.hh \
                         ASServent.hh pipe.hh object.hh joystick.hh

