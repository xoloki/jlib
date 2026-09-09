AM_CPPFLAGS = -I$(top_srcdir) -I$(top_builddir)

# Not TESTS, and not bin_PROGRAMS.
#
# These need a model file, they take minutes, and what they emit is a number
# to read rather than a pass or a fail.  Putting them in TESTS would make
# `make check` slow and make it fail on a machine with no GGUF and no GPU,
# both of which it currently survives.  Putting them in bin_PROGRAMS would
# install measuring tools alongside the library.
#
# noinst_PROGRAMS with a conditional is the arrangement that leaves them
# buildable on request, reviewable in the tree, and out of everything else.
# HAVE_METAL as well as HAVE_PERF: every one of these measures the GPU path,
# and all five include <jlib/metal/backend.hh>.  There is nothing here for a
# machine without Metal to build, and --enable-perf on Linux would otherwise
# fail rather than quietly build nothing.
if HAVE_PERF
if HAVE_METAL
noinst_PROGRAMS = jperf-bench jperf-quick jperf-ops jperf-bwmax jperf-counters

jperf_bench_SOURCES  = bench.cc
jperf_quick_SOURCES  = quick.cc
jperf_ops_SOURCES    = ops.cc

# The two that talk to Metal directly rather than through jlib, because what
# they measure is the device rather than the library.
jperf_bwmax_SOURCES    = bwmax.mm
jperf_counters_SOURCES = counters.mm

AM_CPPFLAGS += -DHAVE_METAL

LDADD = $(top_builddir)/jlib/ai/libjai.la \
        $(top_builddir)/jlib/metal/libjmetal.la \
        $(top_builddir)/jlib/util/libjutil.la \
        $(top_builddir)/jlib/sys/libjsys.la \
        $(METAL_LIBS)

# These two talk to Metal directly and want none of jlib.
jperf_bwmax_LDADD    = $(METAL_LIBS)
jperf_counters_LDADD = $(METAL_LIBS)
endif
endif

EXTRA_DIST = bench.cc quick.cc ops.cc bwmax.mm counters.mm README.md
