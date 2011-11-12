INCLUDES = -I$(top_srcdir)

if HAVE_OSS_AUDIO
LIBJMEDIA = libjmedia.la
else
LIBJMEDIA =
endif

lib_LTLIBRARIES = $(LIBJMEDIA)

libjmedia_la_SOURCES =  Player.cc AudioFile.cc WavFile.cc PlayList.cc Dsp.cc
libjmedia_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjmedia_la_LIBADD = $(top_builddir)/jlib/sys/libjsys.la $(top_builddir)/jlib/util/libjutil.la

libjmediaincludedir=$(includedir)/jlib-1.2/jlib/media
libjmediainclude_HEADERS = Player.hh AudioFile.hh WavFile.hh PlayList.hh stream.hh datastream.hh notestream.hh Dsp.hh Type.hh wavstream.hh audiofilestream.hh
