#!/bin/sh
#
# Build the .deb set, for this machine or for the server.
#
#     tools/package-debs.sh            native (arm64 on this Mac)
#     tools/package-debs.sh amd64      cross, for dancingdragon
#
# Named "package-" rather than "build-" because .gitignore line 13 is
# `build-*`, which is meant for build directories and silently swallowed the
# first spelling of this file -- a script git does not track is one that is
# not there after the next clone.
#
# Output goes to ../debs-<arch>/ beside the checkout, with a STAMP file naming
# the commit it was built from. **That stamp is the point.** "Are these
# packages built with this code?" is a question that has been asked and could
# not be answered without rebuilding, and a .deb carries a version but not a
# revision -- every build so far has been 2.0.0, whatever went into it.
#
# The cross build runs the whole compile under qemu, which is slow -- tens of
# minutes -- and needs binfmt registered. Docker Desktop drops that
# registration when its VM restarts, and the symptom is `exec format error`
# rather than anything about binfmt:
#
#     docker run --privileged --rm tonistiigi/binfmt --install amd64
set -eu

arch=${1:-}
here=$(cd "$(dirname "$0")/.." && pwd)

if [ -n "$arch" ]; then
    platform="--platform linux/$arch"
    tag="jlib-deb-$arch"

    # **LTO off when cross-building.** gcc's lto1 hits an internal compiler
    # error under emulation -- an artefact of qemu rather than of the code,
    # and it does not reproduce natively. debian/rules uses ?= so this wins.
    maint="hardening=+all optimize=-lto"
else
    arch=$(docker version --format '{{.Server.Arch}}' 2>/dev/null || echo unknown)
    platform=""
    tag="jlib-deb-$arch"
    maint="hardening=+all"
fi

out="$here/../debs-$arch"

commit=$(git -C "$here" rev-parse --short HEAD)
branch=$(git -C "$here" rev-parse --abbrev-ref HEAD)
dirty=$(git -C "$here" status --porcelain | head -1)

echo "building $arch packages from $branch $commit"

if [ -n "$dirty" ]; then
    # Not fatal -- sometimes a package is wanted from a work in progress --
    # but it goes in the stamp, because a package built from an uncommitted
    # tree cannot be reproduced from the commit it names.
    echo "  WARNING: the tree has uncommitted changes"
fi

# shellcheck disable=SC2086
docker build -q $platform -t "$tag" "$here" >/dev/null

# **The maintainer options go in as an environment variable, not as an
# `export` inside the command.** The first version built the string
# "DEB_BUILD_MAINT_OPTIONS=hardening=+all optimize=-lto" and ran `export $maint`
# unquoted, so the shell split it on the space: it exported hardening=+all and
# then tried to export a variable named "optimize=-lto". LTO stayed on, and the
# cross build died in lto_main() with the internal compiler error this flag
# exists to avoid -- twenty minutes in, with the flag visibly present in the
# script and visibly absent from the compile line.
# shellcheck disable=SC2086
cid=$(docker create $platform -e DEB_BUILD_MAINT_OPTIONS="$maint" "$tag" sh -c "
    cd /src/jlib &&
    echo \"DEB_BUILD_MAINT_OPTIONS=\$DEB_BUILD_MAINT_OPTIONS\" &&
    dpkg-buildpackage -b -us -uc 2>&1 | tail -40 &&
    mkdir -p /debs && cp /src/*.deb /debs/
")

trap 'docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT INT TERM

rc=0
docker start -a "$cid" || rc=$?

if [ "$rc" -ne 0 ]; then
    echo "build failed (rc=$rc)"
    exit "$rc"
fi

mkdir -p "$out"
rm -f "$out"/*.deb
docker cp "$cid:/debs/." "$out/" >/dev/null

{
    echo "commit:  $commit"
    echo "branch:  $branch"
    echo "arch:    $arch"
    echo "dirty:   ${dirty:-no}"
    echo "subject: $(git -C "$here" log -1 --format=%s)"
} > "$out/STAMP"

echo
echo "$out:"
ls -la "$out"
echo
cat "$out/STAMP"
