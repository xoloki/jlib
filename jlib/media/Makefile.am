AM_CPPFLAGS = -I$(top_srcdir)

# TODO(phase 3): replace the OSS backend with PortAudio and gate on
# HAVE_PORTAUDIO instead of HAVE_OSS_AUDIO.
if HAVE_OSS_AUDIO
LIBJMEDIA = libjmedia.la
endif

lib_LTLIBRARIES = $(LIBJMEDIA)
libjmedia_la_SOURCES = Player.cc AudioFile.cc WavFile.cc PlayList.cc Dsp.cc
libjmedia_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjmedia_la_LIBADD = $(top_builddir)/jlib/sys/libjsys.la \
                      $(top_builddir)/jlib/util/libjutil.la

jmedia_hdrs = Player.hh AudioFile.hh WavFile.hh PlayList.hh stream.hh \
                 datastream.hh notestream.hh Dsp.hh Type.hh wavstream.hh \
                 audiofilestream.hh

libjmediaincludedir = $(includedir)/jlib-1.2/jlib/media
if HAVE_OSS_AUDIO
libjmediainclude_HEADERS = $(jmedia_hdrs)
endif

EXTRA_DIST = $(jmedia_hdrs)
