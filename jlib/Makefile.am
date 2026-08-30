if HAVE_CUDA
cuda_subdirs = cuda
else
cuda_subdirs =
endif

if HAVE_METAL
metal_subdirs = metal
else
metal_subdirs =
endif

SUBDIRS = sys util crypt media net math x gl glu glx glfw ai $(cuda_subdirs) $(metal_subdirs) apps
EXTRA_DIST = jlib-1.2.pc.in

pkgconfigdir		= $(libdir)/pkgconfig
pkgconfig_DATA		= jlib-1.2.pc
