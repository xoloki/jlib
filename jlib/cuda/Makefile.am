INCLUDES = -I$(top_srcdir)

lib_LTLIBRARIES = libjcuda.la
libjcuda_la_SOURCES = cuda.cc #environment.cc agent.cc percept.cc action.cc vacuum.cc
#libjcuda_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
#libjcuda_la_LIBADD = $(top_builddir)/jlib/sys/libjsys.la \
#	                 $(top_builddir)/jlib/crypt/libjcrypt.la
libjcudaincludedir=$(includedir)/jlib-1.2/jlib/cuda
libjcudainclude_HEADERS = cuda.hh gemm.hh neural.hh
