#!/bin/bash
#
# Build and run the coverage-guided fuzzers (#268).
#
#     tests/fuzz/run.sh                    both targets, 600s each
#     tests/fuzz/run.sh 3600               both targets, an hour each
#     tests/fuzz/run.sh 3600 target_path   one target
#
# **Not part of `make check`.** A fuzzer in the suite either runs too briefly
# to find anything or makes the suite slow and non-deterministic, and both are
# worse than not running it. `make fuzz` runs this for long enough to mean
# something; `make check` stays fast and repeatable.
#
# **In the container, because that is the deployment target** -- and because
# Apple's clang does not ship libFuzzer: `libclang_rt.fuzzer_osx.a` is not in
# the Xcode toolchain, so `-fsanitize=fuzzer` links against nothing. Ubuntu's
# clang has it.
#
# **Nothing is mounted.** The code arrives in the image by COPY, and the
# corpus crosses the boundary twice with `docker cp` -- once in, once out --
# rather than living on a bind mount for the duration. See the Dockerfile for
# why: a corpus is thousands of small files and libFuzzer touches them one at
# a time, which is the worst case for macOS's filesystem virtualization.
#
# The corpus lives outside the repo, under CORPUS (default ~/.cache/jlib-fuzz).
# A coverage-guided corpus grows a file per interesting input for as long as
# anyone runs it, and #247 settled that measurements do not belong in the tree.
set -eu

seconds=${1:-600}
only=${2:-}
here=$(cd "$(dirname "$0")/../.." && pwd)
corpus=${CORPUS:-$HOME/.cache/jlib-fuzz}
# Beside the corpus, never inside it: everything under $corpus is shipped
# into the container and fed to libFuzzer, and a crashing input in the corpus
# is replayed at the start of every future run.
crashes=${CRASHES:-$corpus-crashes}

mkdir -p "$corpus" "$crashes"

docker build -q -t jlib-check "$here" >/dev/null

cid=$(docker create jlib-check sh tests/fuzz/inner.sh "$seconds" "$only")

# The container is removed on every exit path, including Ctrl-C. `docker
# create` without this leaks a stopped container per interrupted run.
cleanup() { docker rm -f "$cid" >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM

# Corpus in. One tar stream, not a mount. An empty directory is fine -- the
# first run has only the seeds baked into the image.
# macOS stamps every file with a com.apple.provenance extended attribute, and
# bsdtar carries xattrs into the archive by default. `docker cp` then tries to
# set it inside a Linux container and fails the whole transfer with
# `lsetxattr ... operation not supported` -- so the metadata has to be dropped
# at the tar, not at the other end.
tarflags=""
if tar --no-mac-metadata --version >/dev/null 2>&1; then
    tarflags="--no-mac-metadata --no-xattrs"
elif tar --no-xattrs --version >/dev/null 2>&1; then
    tarflags="--no-xattrs"
fi

if [ -n "$(ls -A "$corpus" 2>/dev/null)" ]; then
    # shellcheck disable=SC2086
    COPYFILE_DISABLE=1 tar $tarflags -C "$corpus" -cf - . | docker cp - "$cid:/corpus"
fi

# **What was already there, so the report can name only what this run found.**
# Counting files in the crash directory answers "are there crashes", which is
# not the question -- a fixed bug leaves its artifact behind, and a run that
# found nothing would go on reporting it forever.
before=$(find "$crashes" -type f 2>/dev/null | sort)

rc=0
docker start -a "$cid" || rc=$?

# **Corpus and crashes out, whatever happened.** This runs after a crash as
# much as after a clean finish, and the crash is the case that matters: the
# saved unit is the entire value of the finding, and it exists only inside a
# container that is about to be removed.
docker cp "$cid:/corpus/." "$corpus/" >/dev/null 2>&1 || true
docker cp "$cid:/artifacts/." "$crashes/" >/dev/null 2>&1 || true

echo
echo "corpus:  $corpus  ($(find "$corpus" -type f | wc -l | tr -d ' ') units)"

fresh=$(comm -13 <(printf '%s\n' "$before") \
                 <(find "$crashes" -type f 2>/dev/null | sort))

if [ -n "$fresh" ]; then
    echo "NEW CRASHES:"
    printf '%s\n' "$fresh" | while IFS= read -r f; do
        [ -n "$f" ] || continue
        echo "  $f"
        echo "    tests/fuzz/repro.sh $(basename "$(dirname "$f")") $f"
    done
else
    old=$(find "$crashes" -type f 2>/dev/null | wc -l | tr -d ' ')
    echo "crashes: none new ($old kept from earlier runs)"
fi

exit "$rc"
