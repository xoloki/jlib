AM_CPPFLAGS = -I$(top_srcdir) $(PORTAUDIO_CFLAGS)

# The OSS backend (Dsp) is kept but no longer built by default: /dev/dsp is
# gone from modern Linux and never existed on macOS.  PortAudio drives the
# device now.
if HAVE_PORTAUDIO
LIBJMEDIA = libjmedia.la
endif

lib_LTLIBRARIES = $(LIBJMEDIA)
libjmedia_la_SOURCES = Player.cc AudioFile.cc WavFile.cc PlayList.cc \
                       AudioSink.cc PortAudioSink.cc
libjmedia_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjmedia_la_LIBADD = $(top_builddir)/jlib/sys/libjsys.la \
                      $(top_builddir)/jlib/util/libjutil.la \
                      $(PORTAUDIO_LIBS)

jmedia_hdrs = Player.hh AudioFile.hh WavFile.hh PlayList.hh stream.hh \
              datastream.hh notestream.hh Type.hh wavstream.hh \
              instrument.hh voice.hh source.hh source_stream.hh wavetable.hh mixer.hh \
              audiofilestream.hh AudioSink.hh PortAudioSink.hh

libjmediaincludedir = $(includedir)/jlib-1.2/jlib/media
if HAVE_PORTAUDIO
libjmediainclude_HEADERS = $(jmedia_hdrs)
endif

EXTRA_DIST = $(jmedia_hdrs) Dsp.hh Dsp.cc
