AM_CPPFLAGS = -I$(top_srcdir) $(CUDA_CFLAGS)

lib_LTLIBRARIES = libjcuda.la
libjcuda_la_SOURCES = cuda.cc
libjcuda_la_LDFLAGS = -version-info $(LIBJ_SO_VERSION) -release $(JLIB_RELEASE) -no-undefined
libjcuda_la_LIBADD = $(CUDA_LIBS)

libjcudaincludedir = $(includedir)/jlib-1.2/jlib/cuda
libjcudainclude_HEADERS = cuda.hh gemm.hh neural.hh
