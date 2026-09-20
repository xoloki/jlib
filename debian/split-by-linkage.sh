#!/bin/sh
#
# Move everything that links GL, X or audio out of one staging tree and into
# another, and then prove that nothing of the kind is left behind.
#
#     split-by-linkage.sh debian/jlib debian/jlib-gl
#
# **Derived rather than enumerated.**  configure builds against whatever the
# machine has, so the same source produces a headless jlib on a bare server
# and a GL-linked one in a container that has every -dev package for the test
# suite.  A hand-written list of which programs are graphical is a list that
# goes stale the first time somebody adds one -- it would land in jlib, put
# libgl1 into a headless server's dependency set, and say nothing until an
# install failed on a machine with no X11.
#
# Asking the binaries what they link cannot drift, because it is the same
# question dpkg-shlibdeps asks when it computes Depends.
set -eu

from=$1
to=$2

# libGL\. and not libGL, so libGLU and libGLEW are matched by their own names
# rather than by accident -- and so a future libGLESv2 is not matched silently.
graphical='NEEDED.*(libGL\.|libGLU|libGLEW|libglfw|libX11|libportaudio)'

# \( -type f -o -type l \) and not -type f -o -type l: without the grouping,
# -o binds looser than the path arguments and the -type l branch would be
# evaluated against every path find knows about rather than these two.
find_files() {
    find "$from/usr/bin" "$from/usr/lib" \( -type f -o -type l \) 2>/dev/null \
        || true
}

moved=0

# **Iterated to a fixed point, because being graphical is transitive.**
#
# jhyper links libjx, and libjx links libX11.  jhyper's own NEEDED entries do
# not mention X at all, so a single pass leaves it in jlib -- and then
# dpkg-shlibdeps gives jlib a dependency on jlib-gl, which drags the whole X11
# stack back in through one more hop.  That is worse than not splitting, since
# it is also circular.
#
# So: move what links GL/X/audio directly, then move what links *that*, and
# keep going until a round moves nothing.

# The SONAMEs already moved, as an alternation for grep.  Rebuilt each round.
moved_sonames() {
    find "$to" -name '*.so*' -type f 2>/dev/null |
        sed 's|.*/||; s|\.so\..*|.so|' | sort -u |
        sed 's|[.+]|\\&|g' | paste -sd'|' -
}

round=0

while : ; do
    round=$((round + 1))
    before=$moved

    # What counts as graphical this round: the real runtimes, plus anything
    # already moved out.
    also=$(moved_sonames)

    if [ -n "$also" ]; then
        pattern="$graphical|NEEDED.*($also)"
    else
        pattern="$graphical"
    fi

    # --- regular files, by what they link ---
    for f in $(find "$from/usr/bin" "$from/usr/lib" -type f 2>/dev/null || true); do
        objdump -p "$f" 2>/dev/null | grep -qE "$pattern" || continue

        rel=${f#"$from"/}

        mkdir -p "$to/$(dirname "$rel")"
        mv "$f" "$to/$rel"

        moved=$((moved + 1))
    done

    # --- symlinks whose target went with them ---
    #
    # A shared library ships as a real file plus SONAME links.  Move the file
    # first and the links dangle; objdump then fails on them, so they match
    # nothing and stay behind -- leaving a package with a library and no
    # SONAME link, which dpkg-shlibdeps rejects, and dangling links that no
    # objdump-based check can see.
    for pass in 1 2; do
        for l in $(find "$from/usr/bin" "$from/usr/lib" -type l 2>/dev/null || true); do
            rel=${l#"$from"/}
            here=$(dirname "$rel")
            target=$(readlink "$l")

            [ -e "$from/$here/$target" ] && continue
            [ -e "$to/$here/$target" ] || continue

            mkdir -p "$to/$here"
            mv "$l" "$to/$rel"

            moved=$((moved + 1))
        done
    done

    [ "$moved" = "$before" ] && break

    echo "split-by-linkage: round $round moved $((moved - before))"
done

echo "split-by-linkage: $moved file(s) moved from $from to $to"

# The check, and the reason this script exits non-zero rather than warning: a
# jlib that still contains one GL-linked file gets libgl1 back in its Depends,
# and the symptom is an install failure on somebody else's machine.
#
# `|| true` on the substitution because the loop's last grep returns non-zero
# whenever the last file examined is not graphical -- which is the ordinary
# case, and which under `set -e` aborted this check on success.
left=$(find_files | while read -r f; do
    if objdump -p "$f" 2>/dev/null | grep -qE "$graphical"; then echo "$f"; fi
done || true)

if [ -n "$left" ]; then
    echo "split-by-linkage: $from still contains files linking GL, X or audio:"
    echo "$left"

    exit 1
fi

echo "split-by-linkage: nothing in $from links GL, X or audio"
