ACLOCAL_AMFLAGS = -I m4

SUBDIRS = jlib tests tools/perf

EXTRA_DIST = autogen.sh DOCKER TODO.md LICENSE NOTICE

# The architecture documents, which are the map the headers cannot be: each
# one spans files, and none of it belongs at any single declaration.
doc_DATA = docs/async.md
EXTRA_DIST += $(doc_DATA)

# The coverage-guided fuzzers (#268).
#
# **Deliberately not in TESTS, and so not in `make check`.** A fuzzer given a
# few seconds finds nothing and reports success, which is worse than not
# running it; a fuzzer given long enough to matter makes the suite slow and
# its result depend on how long it ran. `make check` stays fast and
# repeatable, and this runs for long enough to mean something:
#
#     make fuzz                              600s per target
#     make fuzz FUZZ_SECONDS=3600            an hour per target
#     make fuzz FUZZ_TARGET=target_path      just the one
#
# The corpus persists between runs under ~/.cache/jlib-fuzz, so a long run
# starts from everything every earlier run learned rather than from the seeds.
FUZZ_SECONDS = 600
FUZZ_TARGET =

.PHONY: fuzz
fuzz:
	$(top_srcdir)/tests/fuzz/run.sh $(FUZZ_SECONDS) $(FUZZ_TARGET)

EXTRA_DIST += tests/fuzz
