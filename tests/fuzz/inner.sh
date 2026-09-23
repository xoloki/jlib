#!/bin/sh
#
# The container half of tests/fuzz/run.sh. **A file in the tree rather than a
# heredoc mounted at run time**: the Dockerfile copies tests/, so this arrives
# in the image with everything else and nothing whatever is mounted.
#
# Bind mounts on Docker Desktop are slow, and code that crosses one is code
# the image does not describe -- an image that builds today and behaves
# differently tomorrow because a file outside it changed.
set -eu
seconds=$1
only=$2
unit=${3:-}   # set: replay this one input instead of fuzzing (see repro.sh)

cd /src/jlib

status=0

for target in head_request grammar_head response_head target_path abnf_rules body_reader; do
    [ -n "$only" ] && [ "$only" != "$target" ] && continue

    echo "=== $target ==="

    mkdir -p "/corpus/$target" "/artifacts/$target"

    # Seeds are copied rather than used in place: libFuzzer writes new units
    # into the first corpus directory it is given, and the seeds are source.
    # -n so a seed never overwrites a unit the corpus already minimised.
    cp -n "tests/fuzz/seeds/$target/"* "/corpus/$target/" 2>/dev/null || true

    # -fsanitize=address as well as fuzzer: the invariant a fuzz target can
    # assert is narrow, and the sanitizer supplies everything it cannot see.
    if ! clang++ -std=c++20 -g -O1 \
        -fsanitize=fuzzer,address \
        -I. -Ibuild \
        "tests/fuzz/$target.cc" \
        -o "/tmp/fuzz_$target" \
        build/jlib/net/.libs/libjnet.a \
        build/jlib/util/.libs/libjutil.a \
        build/jlib/sys/.libs/libjsys.a \
        build/jlib/crypt/.libs/libjcrypt.a \
        -lssl -lcrypto -lsodium -lpthread
    then
        echo "  $target: BUILD FAILED"
        status=1
        continue
    fi

    if [ -n "$unit" ]; then
        # Replay. No corpus, no mutation, no time limit -- just this input,
        # with the sanitizer and the invariants live.
        if "/tmp/fuzz_$target" "$unit"; then
            echo "  $target: input does NOT reproduce a failure"
        else
            echo "  $target: reproduced (rc=$?)"
            status=1
        fi
        continue
    fi

    # **The output is not filtered.** An earlier version piped this through a
    # grep for the lines that looked interesting, which is how a crash report
    # went missing: the run died, the filter matched nothing, and what reached
    # the log was a single INITED line that read like a healthy start.
    #
    # -artifact_prefix is what decides where a crashing input is written, and
    # it must not be the corpus: a saved crash in the corpus is replayed at
    # the start of every subsequent run, so the next run dies on it forever.
    if "/tmp/fuzz_$target" "/corpus/$target" \
        -max_total_time="$seconds" \
        -print_final_stats=1 \
        -timeout=5 \
        -artifact_prefix="/artifacts/$target/"
    then
        echo "  $target: clean"
    else
        echo "  $target: CRASHED (rc=$?) -- see the saved unit"
        status=1
    fi
done

exit "$status"
