INCLUDES = -I$(top_srcdir) -I$(top_builddir)

if HAVE_CUDA
cuda_subdirs = cuda
else
cuda_subdirs = 
endif


SUBDIRS = sys util crypt media net math x gl glu glx glut ai $(cuda_subdirs) apps 
EXTRA_DIST = jlib-1.2.pc.in

pkgconfigdir		= $(libdir)/pkgconfig
pkgconfig_DATA		= jlib-1.2.pc

