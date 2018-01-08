INCLUDES = -I$(top_srcdir)  -I/usr/local/cuda/include

if HAVE_CUDA
cuda_programs = jcublas
else
cuda_programs = 
endif

bin_PROGRAMS = jlib-mail jpoisoned jjoystick jhyper jhardhyper jglxhyper jgluthyper jgltorus jglxbox jjoy2xev jcrypt jnote jneural-zero jneural-alpha jneural-search $(cuda_programs)

dist_pkgdata_DATA = wow-buttons-ps3.map wow-axes-ps3.map wow-buttons-x360.map wow-axes-x360.map 

jlib_mail_SOURCES = jlib-mail.cc
jlib_mail_LDADD = $(top_builddir)/jlib/crypt/libjcrypt.la \
	$(top_builddir)/jlib/net/libjnet.la \
	$(top_builddir)/jlib/sys/libjsys.la \
	$(top_builddir)/jlib/util/libjutil.la 

jpoisoned_SOURCES = jpoisoned.cc
jpoisoned_LDADD = $(top_builddir)/jlib/x/libjx.la \
	$(top_builddir)/jlib/sys/libjsys.la \
	$(top_builddir)/jlib/util/libjutil.la 

jjoystick_SOURCES = jjoystick.cc
jjoystick_LDADD = $(top_builddir)/jlib/x/libjx.la \
	$(top_builddir)/jlib/sys/libjsys.la \
	$(top_builddir)/jlib/util/libjutil.la 

jjoy2xev_SOURCES = jjoy2xev.cc
jjoy2xev_LDADD = $(top_builddir)/jlib/x/libjx.la \
	$(top_builddir)/jlib/sys/libjsys.la \
	$(top_builddir)/jlib/util/libjutil.la 

jhyper_SOURCES = jhyper.cc
jhyper_LDADD = $(top_builddir)/jlib/x/libjx.la \
	$(top_builddir)/jlib/x/libjx.la \
	$(top_builddir)/jlib/util/libjutil.la

jglxhyper_SOURCES = jglxhyper.cc Hyper.hh
jglxhyper_LDADD = $(top_builddir)/jlib/glx/libjglx.la  \
	$(top_builddir)/jlib/x/libjx.la  \
	$(top_builddir)/jlib/util/libjutil.la -lGL -lGLU

jgluthyper_SOURCES = jgluthyper.cc
jgluthyper_LDADD = $(top_builddir)/jlib/x/libjx.la $(top_builddir)/jlib/glut/libjglut.la -lGL -lGLU -lglut

jhardhyper_SOURCES = jhardhyper.cc
jhardhyper_LDADD = $(top_builddir)/jlib/glut/libjglut.la -lGL -lGLU -lglut

jgltorus_SOURCES = jgltorus.cc
jgltorus_LDADD = $(top_builddir)/jlib/glut/libjglut.la -lGL -lGLU -lglut

jglxbox_SOURCES = jglxbox.cc
jglxbox_LDADD = $(top_builddir)/jlib/glx/libjglx.la \
	$(top_builddir)/jlib/glu/libjglu.la \
	$(top_builddir)/jlib/gl/libjgl.la \
	$(top_builddir)/jlib/x/libjx.la -lGL -lGLU

jcrypt_SOURCES = jcrypt.cc
jcrypt_LDADD = $(top_builddir)/jlib/crypt/libjcrypt.la \
	$(top_builddir)/jlib/sys/libjsys.la

jnote_SOURCES = jnote.cc
jnote_LDADD = $(top_builddir)/jlib/media/libjmedia.la \
	$(top_builddir)/jlib/sys/libjsys.la

jneural_zero_SOURCES = jneural-zero.cc
jneural_zero_LDADD = $(top_builddir)/jlib/util/libjutil.la \
	$(top_builddir)/jlib/sys/libjsys.la

jneural_alpha_SOURCES = jneural-alpha.cc
jneural_alpha_LDADD = $(top_builddir)/jlib/util/libjutil.la \
	$(top_builddir)/jlib/sys/libjsys.la

jneural_search_SOURCES = jneural-search.cc
jneural_search_LDADD = $(top_builddir)/jlib/util/libjutil.la \
	$(top_builddir)/jlib/sys/libjsys.la

jcublas_SOURCES = jcublas.cc
jcublas_LDADD = $(top_builddir)/jlib/util/libjutil.la \
	$(top_builddir)/jlib/sys/libjsys.la \
	$(top_builddir)/jlib/cuda/libjcuda.la \
	-L/usr/local/cuda/lib64 -lcublas -lcudart
