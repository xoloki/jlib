INCLUDES = -I$(top_srcdir)

lib_LTLIBRARIES = libjcrypt.la
libjcrypt_la_SOURCES = crypt.cc curve.cc groth.cc schnorr.cc
libjcrypt_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjcrypt_la_LIBADD = $(top_builddir)/jlib/sys/libjsys.la $(top_builddir)/jlib/util/libjutil.la
libjcryptincludedir=$(includedir)/jlib-1.2/jlib/crypt

libjcryptinclude_HEADERS = crypt.hh curve.hh groth.hh schnorr.hh
