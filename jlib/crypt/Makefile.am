AM_CPPFLAGS = -I$(top_srcdir)

lib_LTLIBRARIES = libjcrypt.la
libjcrypt_la_SOURCES = crypt.cc groth.cc
libjcrypt_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjcrypt_la_LIBADD = $(top_builddir)/jlib/sys/libjsys.la $(top_builddir)/jlib/util/libjutil.la
libjcryptincludedir=$(includedir)/jlib-1.2/jlib/crypt

libjcryptinclude_HEADERS = crypt.hh

TESTS = \
	groth_test

groth_test_SOURCES = tests/groth_test.cc
groth_test_LDADD = $(top_builddir)/jlib/util/libjutil.la $(top_builddir)/jlib/crypt/libcrypt.la
