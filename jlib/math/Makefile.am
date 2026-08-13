AM_CPPFLAGS = -I$(top_srcdir)

# jlib/math is header-only.  It used to declare a lib_LTLIBRARIES whose
# _SOURCES were all .hh files, which builds no objects and cannot be linked.

libjmathincludedir = $(includedir)/jlib-1.2/jlib/math
libjmathinclude_HEADERS = math.hh dump.hh buffer.hh tensor.hh matrix.hh Plot.hh polynomial.hh
