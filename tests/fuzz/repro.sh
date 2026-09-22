#!/bin/sh
#
# Replay one saved input against one fuzz target.
#
#     tests/fuzz/repro.sh target_path ~/.cache/jlib-fuzz/crashes/target_path/crash-abc123
#
# The point of a saved unit is that a finding outlives the run that found it.
# This is the other half of that: no corpus, no mutation, no time limit, just
# the input and the invariants, so a fix can be shown to change the answer.
set -eu

target=${1:?usage: repro.sh <target> <file>}
file=${2:?usage: repro.sh <target> <file>}
here=$(cd "$(dirname "$0")/../.." && pwd)

[ -f "$file" ] || { echo "no such file: $file" >&2; exit 2; }

docker build -q -t jlib-check "$here" >/dev/null

cid=$(docker create jlib-check sh tests/fuzz/inner.sh 0 "$target" /artifacts/unit)
trap 'docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT INT TERM

docker cp "$file" "$cid:/artifacts/unit" >/dev/null

rc=0
docker start -a "$cid" || rc=$?
exit "$rc"
