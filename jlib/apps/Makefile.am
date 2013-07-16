INCLUDES = -I$(top_srcdir)

bin_PROGRAMS = jlib-mail jpoisoned jjoystick jhyper jhardhyper jglxhyper jgluthyper jgltorus jglxbox jjoy2xev jcrypt jnote

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
	$(top_builddir)/jlib/x/libjx.la

jglxhyper_SOURCES = jglxhyper.cc Hyper.hh
jglxhyper_LDADD = $(top_builddir)/jlib/glx/libjglx.la  \
	$(top_builddir)/jlib/x/libjx.la

jgluthyper_SOURCES = jgluthyper.cc
jgluthyper_LDADD = $(top_builddir)/jlib/glut/libjglut.la 

jhardhyper_SOURCES = jhardhyper.cc
jhardhyper_LDADD = $(top_builddir)/jlib/glut/libjglut.la 

jgltorus_SOURCES = jgltorus.cc
jgltorus_LDADD = $(top_builddir)/jlib/glut/libjglut.la 

jglxbox_SOURCES = jglxbox.cc
jglxbox_LDADD = $(top_builddir)/jlib/glx/libjglx.la \
	$(top_builddir)/jlib/glu/libjglu.la \
	$(top_builddir)/jlib/gl/libjgl.la \
	$(top_builddir)/jlib/x/libjx.la

jcrypt_SOURCES = jcrypt.cc
jcrypt_LDADD = $(top_builddir)/jlib/crypt/libjcrypt.la \
	$(top_builddir)/jlib/sys/libjsys.la

jnote_SOURCES = jnote.cc
jnote_LDADD = $(top_builddir)/jlib/media/libjmedia.la \
	$(top_builddir)/jlib/sys/libjsys.la
