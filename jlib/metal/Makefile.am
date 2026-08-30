# Objective-C++, because Metal's API is Objective-C.  The headers installed
# here are plain C++ -- see device.hh -- so nothing that includes them has to
# be compiled as Objective-C++.
AM_CPPFLAGS = -I$(top_srcdir)

lib_LTLIBRARIES = libjmetal.la

libjmetal_la_SOURCES = device.mm gemm.mm tensor.mm
noinst_HEADERS       = impl.hh
libjmetal_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjmetal_la_LIBADD  = $(METAL_LIBS)

# ARC, so the Objective-C objects behind the opaque handles are released
# without a retain/release in every path.
libjmetal_la_OBJCXXFLAGS = $(AM_OBJCXXFLAGS) -fobjc-arc -std=c++20

libjmetalincludedir = $(includedir)/jlib-1.2/jlib/metal
libjmetalinclude_HEADERS = device.hh gemm.hh tensor.hh
