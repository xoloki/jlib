AM_CPPFLAGS = -I$(top_srcdir) $(GPGME_CFLAGS) $(GPGERROR_CFLAGS) $(SODIUM_CFLAGS)

if HAVE_SODIUM
LIBJCRYPT_CURVE = curve.cc groth.cc schnorr.cc
CURVE_LIBS = $(SODIUM_LIBS)
else
LIBJCRYPT_CURVE =
CURVE_LIBS =
endif

lib_LTLIBRARIES = libjcrypt.la
libjcrypt_la_SOURCES = crypt.cc $(LIBJCRYPT_CURVE)
libjcrypt_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjcrypt_la_LIBADD = $(top_builddir)/jlib/sys/libjsys.la \
                      $(top_builddir)/jlib/util/libjutil.la \
                      $(GPGME_LIBS) $(GPGERROR_LIBS) $(CURVE_LIBS)

libjcryptincludedir = $(includedir)/jlib-1.2/jlib/crypt
libjcryptinclude_HEADERS = crypt.hh curve.hh groth.hh schnorr.hh
