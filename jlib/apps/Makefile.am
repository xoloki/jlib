# Public jlib headers include <gpgme.h>, <sodium.h>, <openssl/*.h> and
# <json.h> directly, so anything compiling against them needs those -I flags.
AM_CPPFLAGS = -I$(top_srcdir) \
	$(GPGME_CFLAGS) $(GPGERROR_CFLAGS) $(SODIUM_CFLAGS) $(OPENSSL_CFLAGS) $(JSONC_CFLAGS)

# Every program below is gated on the conditionals its _LDADD needs.  The
# libraries themselves are conditional, so an ungated program would fail at
# link rather than being skipped cleanly.

if HAVE_SODIUM
curve_programs = jcurve
endif

if HAVE_MAGICK
neural_programs = jneural-zero jneural-alpha jneural-search
endif

if HAVE_CUDA
cuda_programs = jcublas
endif

if HAVE_OSS_AUDIO
media_programs = jnote jmelody
endif

if HAVE_X
x_programs = jpoisoned jhyper
if HAVE_JOYSTICK
xjoy_programs = jjoy2xev
endif
endif

if HAVE_JOYSTICK
joy_programs = jjoystick
endif

if HAVE_GLX
glx_programs = jglxhyper jglxbox
endif

if HAVE_GLUT
glut_programs = jgluthyper jhardhyper jgltorus
endif

bin_PROGRAMS = jlib-mail jcrypt jm3u \
	$(curve_programs) \
	$(neural_programs) \
	$(cuda_programs) \
	$(media_programs) \
	$(x_programs) \
	$(xjoy_programs) \
	$(joy_programs) \
	$(glx_programs) \
	$(glut_programs)

dist_pkgdata_DATA = wow-buttons-ps3.map wow-axes-ps3.map \
                    wow-buttons-x360.map wow-axes-x360.map \
                    ksp-axes-ps3.map ksp-buttons-ps3.map \
                    tor-axes-ps3.map tor-buttons-ps3.map

EXTRA_DIST = Hyper.hh

JCRYPT_LA  = $(top_builddir)/jlib/crypt/libjcrypt.la
JNET_LA    = $(top_builddir)/jlib/net/libjnet.la
JSYS_LA    = $(top_builddir)/jlib/sys/libjsys.la
JUTIL_LA   = $(top_builddir)/jlib/util/libjutil.la
JX_LA      = $(top_builddir)/jlib/x/libjx.la
JGL_LA     = $(top_builddir)/jlib/gl/libjgl.la
JGLU_LA    = $(top_builddir)/jlib/glu/libjglu.la
JGLX_LA    = $(top_builddir)/jlib/glx/libjglx.la
JGLUT_LA   = $(top_builddir)/jlib/glut/libjglut.la
JMEDIA_LA  = $(top_builddir)/jlib/media/libjmedia.la
JCUDA_LA   = $(top_builddir)/jlib/cuda/libjcuda.la

jlib_mail_SOURCES = jlib-mail.cc
jlib_mail_LDADD   = $(JCRYPT_LA) $(JNET_LA) $(JSYS_LA) $(JUTIL_LA)

jcrypt_SOURCES = jcrypt.cc
jcrypt_LDADD   = $(JCRYPT_LA) $(JSYS_LA)

jm3u_SOURCES = jm3u.cc
jm3u_LDADD   = $(JSYS_LA) $(JUTIL_LA)

jcurve_SOURCES = jcurve.cc
jcurve_LDADD   = $(JCRYPT_LA) $(JUTIL_LA) $(JSYS_LA) $(SODIUM_LIBS)

jneural_zero_SOURCES   = jneural-zero.cc
jneural_zero_CPPFLAGS  = $(AM_CPPFLAGS) $(MAGICK_CFLAGS)
jneural_zero_LDADD     = $(JUTIL_LA) $(JSYS_LA) $(MAGICK_LIBS)

jneural_alpha_SOURCES  = jneural-alpha.cc
jneural_alpha_CPPFLAGS = $(AM_CPPFLAGS) $(MAGICK_CFLAGS)
jneural_alpha_LDADD    = $(JUTIL_LA) $(JSYS_LA) $(MAGICK_LIBS)

jneural_search_SOURCES  = jneural-search.cc
jneural_search_CPPFLAGS = $(AM_CPPFLAGS) $(MAGICK_CFLAGS)
jneural_search_LDADD    = $(JUTIL_LA) $(JSYS_LA) $(MAGICK_LIBS)

jcublas_SOURCES  = jcublas.cc
jcublas_CPPFLAGS = $(AM_CPPFLAGS) $(CUDA_CFLAGS) $(MAGICK_CFLAGS)
jcublas_LDADD    = $(JUTIL_LA) $(JSYS_LA) $(JCUDA_LA) $(CUDA_LIBS) $(MAGICK_LIBS)

jnote_SOURCES = jnote.cc
jnote_LDADD   = $(JMEDIA_LA) $(JSYS_LA)

jmelody_SOURCES = jmelody.cc
jmelody_LDADD   = $(JMEDIA_LA) $(JSYS_LA)

jpoisoned_SOURCES = jpoisoned.cc
jpoisoned_LDADD   = $(JX_LA) $(JSYS_LA) $(JUTIL_LA)

jhyper_SOURCES = jhyper.cc
jhyper_LDADD   = $(JX_LA) $(JUTIL_LA)

# jjoystick uses jlib/sys/joystick.hh only; it has no X11 dependency.
jjoystick_SOURCES = jjoystick.cc
jjoystick_LDADD   = $(JSYS_LA) $(JUTIL_LA)

jjoy2xev_SOURCES = jjoy2xev.cc
jjoy2xev_LDADD   = $(JX_LA) $(JSYS_LA) $(JUTIL_LA)

jglxhyper_SOURCES = jglxhyper.cc
jglxhyper_LDADD   = $(JGLX_LA) $(JX_LA) $(JUTIL_LA) $(GLU_LIBS) $(GL_LIBS)

jglxbox_SOURCES = jglxbox.cc
jglxbox_LDADD   = $(JGLX_LA) $(JGLU_LA) $(JGL_LA) $(JX_LA) $(GLU_LIBS) $(GL_LIBS)

jgluthyper_SOURCES = jgluthyper.cc
jgluthyper_LDADD   = $(JGLUT_LA) $(GLUT_LIBS) $(GLU_LIBS) $(GL_LIBS)

jhardhyper_SOURCES = jhardhyper.cc
jhardhyper_LDADD   = $(JGLUT_LA) $(GLUT_LIBS) $(GLU_LIBS) $(GL_LIBS)

jgltorus_SOURCES = jgltorus.cc
jgltorus_LDADD   = $(JGLUT_LA) $(GLUT_LIBS) $(GLU_LIBS) $(GL_LIBS)
